#include "vulkan_include.hh"
#include "vkintox_internal.hh"

#include <mutex>
#include <map>
#include <set>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <string>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <dlfcn.h>
#include <sys/stat.h>
#include <signal.h>
#include <setjmp.h>
#include <execinfo.h>
#include <limits>

#include "util.hh"
#include "keyboard_input.hh"
#include "keyboard_input_wayland.hh"
#include "mouse_input_wayland.hh"
#include "wayland_input_common.hh"
#include "input_blocker.hh"
#include "wayland_display.hh"

#include "logical_device.hh"
#include "vk_handle.hh"
#include "logical_swapchain.hh"

#include "image_view.hh"
#include "image.hh"
#include "sampler.hh"
#include "framebuffer.hh"
#include "descriptor_set.hh"
#include "shader.hh"
#include "graphics_pipeline.hh"
#include "command_buffer.hh"
#include "buffer.hh"
#include "memory.hh"
#include "config.hh"
#include "config_serializer.hh"
#include "settings_manager.hh"
#include "fake_swapchain.hh"
#include "renderpass.hh"
#include "format.hh"
#include "logger.hh"
#include "shader_sources.hh"

#include "effects/effect.hh"
#include "effects/effect_reshade.hh"
#include "effects/effect_transfer.hh"
#include "imgui_overlay.hh"
#include "effects/effect_registry.hh"

// Vulkan platform surface interception signatures used in this translation unit.
// Keep these after project headers to avoid X11 macro collisions in shared headers.
#define VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_XCB_KHR
#include <X11/Xlib.h>
#include <xcb/xcb.h>
#include <wayland-client.h>
#include "vulkan/vulkan_wayland.h"
#include "vulkan/vulkan_xlib.h"
#include "vulkan/vulkan_xcb.h"

#define VKINTOX_LAYER_NAME "VK_LAYER_VKINTOX_post_processing"

#if defined(__GNUC__) && __GNUC__ >= 4
#define VK_SHADE_EXPORT __attribute__((visibility("default")))
#else
#error "Unsupported platform!"
#endif

namespace VKIntox
{
    std::shared_ptr<Config> baseConfig = nullptr;  // Always VKIntox.conf
    std::shared_ptr<Config> config = nullptr;      // Current config (base + overlay)
    EffectRegistry effectRegistry;                   // Single source of truth for effect configs

    static std::once_flag initConfigsOnceFlag;

    // layer book-keeping information, to store dispatch tables by key
    std::unordered_map<void*, InstanceDispatch>                           instanceDispatchMap;
    std::unordered_map<void*, VkInstance>                                 instanceMap;
    std::unordered_map<void*, uint32_t>                                   instanceVersionMap;
    std::unordered_map<void*, std::shared_ptr<LogicalDevice>>             deviceMap;
    std::unordered_map<VkSwapchainKHR, std::shared_ptr<LogicalSwapchain>> swapchainMap;

    // Cache: VkRenderPass → depth attachment's finalLayout.
    // Populated at CreateRenderPass time. Used by v3 deferred copy to
    // determine the correct source image layout for the copy barrier.
    static std::unordered_map<VkRenderPass, VkImageLayout> renderPassDepthFinalLayouts;

    std::mutex globalLock;
    using scoped_lock = std::lock_guard<std::mutex>;

    // DepthRetryState and depthRetryStates live in vkintox_internal.hh, since
    // the depth subsystem is what arms and polls the retries.
    std::unordered_map<LogicalDevice*, DepthRetryState> depthRetryStates;

    static std::mutex deviceLossLock;
    static std::unordered_set<LogicalDevice*> deviceLostDevices;

    template<typename DispatchableType>
    void* GetKey(DispatchableType inst)
    {
        return *(void**) inst;
    }

    // Cached available effects data (to avoid re-parsing config every frame)
    struct CachedEffectsData
    {
        std::vector<std::string> currentConfigEffects;
        std::vector<std::string> defaultConfigEffects;
        std::map<std::string, std::string> effectPaths;
        std::string configPath;
        bool initialized = false;
    };
    CachedEffectsData cachedEffects;

    // Cached parameters (to avoid re-parsing config every frame)
    struct CachedParametersData
    {
        std::vector<std::unique_ptr<EffectParam>> parameters;
        std::vector<std::string> effectNames;  // Effects when params were collected
        std::string configPath;
        bool dirty = true;  // Set to true to force recollection
    };
    CachedParametersData cachedParams;

    static bool pNextChainContainsSType(const void* pNext, VkStructureType sType)
    {
        const VkBaseInStructure* base = reinterpret_cast<const VkBaseInStructure*>(pNext);
        while (base != nullptr)
        {
            if (base->sType == sType)
                return true;
            base = base->pNext;
        }
        return false;
    }

    std::string formatHexU64(uint64_t value)
    {
        std::ostringstream oss;
        oss << std::hex << value;
        return oss.str();
    }

    // Debounce for resize - delays effect reload until resize stops
    struct ResizeDebounceState
    {
        std::chrono::steady_clock::time_point lastResizeTime;
        bool pending = false;
    };
    ResizeDebounceState resizeDebounce;
    constexpr int64_t RESIZE_DEBOUNCE_MS = 200;

    // deferred startup REMOVED, caused race conditions :<
    struct DeferredStartupReload
    {
        bool   fired      = true;   // Always fired (disabled)
    };
    DeferredStartupReload deferredStartupReload;

    static bool parseBoolEnvValue(const std::string& value, bool defaultValue)
    {
        std::string s(value);
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (s == "1" || s == "true" || s == "yes" || s == "on")
            return true;
        if (s == "0" || s == "false" || s == "no" || s == "off")
            return false;
        return defaultValue;
    }

    static bool getMutableSwapchainEnvOverride(bool& hasOverride, bool defaultValue)
    {
        const char* value = std::getenv("VKINTOX_ENABLE_MUTABLE_SWAPCHAIN");
        if (value == nullptr)
        {
            hasOverride = false;
            return defaultValue;
        }

        hasOverride = true;
        return parseBoolEnvValue(value, defaultValue);
    }

    static bool isSwapchainDiagEnabled()
    {
        const char* value = std::getenv("VKINTOX_DIAG_SWAPCHAIN");
        if (value == nullptr)
            return false;
        return parseBoolEnvValue(value, false);
    }

    static bool isGpuCrashDiagEnabled()
    {
        const char* value = std::getenv("VKINTOX_GPU_CRASH_DIAGNOSTICS");
        if (value == nullptr)
            return false;
        return parseBoolEnvValue(value, false);
    }

#ifdef VKINTOX_DEBUG
    static bool isDepthCopyDumpEnabled()
    {
        const char* value = std::getenv("VKINTOX_DEBUG_DUMP_DEPTH_COPY");
        if (value == nullptr)
            return false;
        return parseBoolEnvValue(value, false);
    }

    static VkImageLayout getInternalDepthReadOnlyLayoutForDebug(VkFormat format)
    {
        return isStencilFormat(format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                       : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    }

    static VkImageLayout getDepthResolveReadOnlyLayoutForDebug(const LogicalSwapchain* logicalSwapchain)
    {
        if (!logicalSwapchain)
            return VK_IMAGE_LAYOUT_UNDEFINED;

        return logicalSwapchain->depthResolveUsesShader
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : getInternalDepthReadOnlyLayoutForDebug(logicalSwapchain->depthResolveFormat);
    }

    static VkImageAspectFlags getDepthResolveAspectMaskForDebug(const LogicalSwapchain* logicalSwapchain)
    {
        if (!logicalSwapchain)
            return VK_IMAGE_ASPECT_COLOR_BIT;

        return logicalSwapchain->depthResolveUsesShader ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    }

    // Developer-only debug helper: dumps the depth-resolve image to
    // /tmp/vkintox-depth-copy-<i>.f32 so we can inspect what the layer is
    // feeding into effects. Compiled out of release builds — it does a
    // synchronous QueueWaitIdle + MapMemory on the present path.
    static void maybeDumpDepthResolveImage(LogicalDevice* logicalDevice, LogicalSwapchain* logicalSwapchain, uint32_t imageIndex, VkQueue queue)
    {
        static bool dumped = false;
        if (dumped || !isDepthCopyDumpEnabled() || !logicalDevice || !logicalSwapchain)
            return;
        if (imageIndex >= logicalSwapchain->depthResolvePerImage.size())
            return;
        if (logicalSwapchain->depthResolveFormat != VK_FORMAT_D32_SFLOAT
            && logicalSwapchain->depthResolveFormat != VK_FORMAT_R32_SFLOAT)
        {
            Logger::warn("depth copy dump only supports VK_FORMAT_D32_SFLOAT or VK_FORMAT_R32_SFLOAT right now, got format="
                         + convertToString(logicalSwapchain->depthResolveFormat));
            dumped = true;
            return;
        }

        VkExtent3D extent = logicalSwapchain->depthResolveExtent;
        if (extent.width == 0 || extent.height == 0)
            return;

        dumped = true;

        // One-shot debug path: wait for prior work so the sampled backup image is stable.
        logicalDevice->vkd.QueueWaitIdle(queue);

        const VkDeviceSize dumpSize = static_cast<VkDeviceSize>(extent.width) * static_cast<VkDeviceSize>(extent.height) * sizeof(float);
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        createBuffer(logicalDevice,
                     dumpSize,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingBuffer,
                     stagingMemory);

        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = logicalDevice->commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkResult vr = logicalDevice->vkd.AllocateCommandBuffers(logicalDevice->device, &allocInfo, &commandBuffer);
        ASSERT_VULKAN(vr);
        initializeDispatchTable(commandBuffer, logicalDevice->device);

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vr = logicalDevice->vkd.BeginCommandBuffer(commandBuffer, &beginInfo);
        ASSERT_VULKAN(vr);

        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.image = logicalSwapchain->depthResolvePerImage[imageIndex].image;
        barrier.oldLayout = getDepthResolveReadOnlyLayoutForDebug(logicalSwapchain);
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask = getDepthResolveAspectMaskForDebug(logicalSwapchain);
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &barrier);

        VkBufferImageCopy region = {};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = getDepthResolveAspectMaskForDebug(logicalSwapchain);
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = extent.width;
        region.imageExtent.height = extent.height;
        region.imageExtent.depth = 1;

        logicalDevice->vkd.CmdCopyImageToBuffer(commandBuffer,
                                                 logicalSwapchain->depthResolvePerImage[imageIndex].image,
                                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 stagingBuffer,
                                                 1,
                                                 &region);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = getDepthResolveReadOnlyLayoutForDebug(logicalSwapchain);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &barrier);

        vr = logicalDevice->vkd.EndCommandBuffer(commandBuffer);
        ASSERT_VULKAN(vr);

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        vr = logicalDevice->vkd.QueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        ASSERT_VULKAN(vr);
        logicalDevice->vkd.QueueWaitIdle(queue);

        void* mapped = nullptr;
        vr = logicalDevice->vkd.MapMemory(logicalDevice->device, stagingMemory, 0, dumpSize, 0, &mapped);
        ASSERT_VULKAN(vr);

        const float* values = static_cast<const float*>(mapped);
        const size_t pixelCount = static_cast<size_t>(extent.width) * static_cast<size_t>(extent.height);
        float minValue = std::numeric_limits<float>::infinity();
        float maxValue = -std::numeric_limits<float>::infinity();
        size_t finiteCount = 0;
        size_t zeroishCount = 0;
        size_t oneishCount = 0;
        for (size_t i = 0; i < pixelCount; ++i)
        {
            const float v = values[i];
            if (!std::isfinite(v))
                continue;
            finiteCount++;
            minValue = std::min(minValue, v);
            maxValue = std::max(maxValue, v);
            if (std::abs(v) < 1e-6f)
                zeroishCount++;
            if (std::abs(v - 1.0f) < 1e-6f)
                oneishCount++;
        }

        auto sampleAt = [&](uint32_t x, uint32_t y) -> float {
            return values[static_cast<size_t>(y) * static_cast<size_t>(extent.width) + x];
        };

        const uint32_t centerX = extent.width / 2;
        const uint32_t centerY = extent.height / 2;
        Logger::warn("depth copy dump: imageIndex=" + std::to_string(imageIndex)
                     + " extent=" + std::to_string(extent.width) + "x" + std::to_string(extent.height)
                     + " finite=" + std::to_string(finiteCount)
                     + " min=" + std::to_string(minValue)
                     + " max=" + std::to_string(maxValue)
                     + " zeroish=" + std::to_string(zeroishCount)
                     + " oneish=" + std::to_string(oneishCount)
                     + " sample(0,0)=" + std::to_string(sampleAt(0, 0))
                     + " sample(center)=" + std::to_string(sampleAt(centerX, centerY))
                     + " sample(last,last)=" + std::to_string(sampleAt(extent.width - 1, extent.height - 1)));

        const std::string dumpPath = "/tmp/vkintox-depth-copy-" + std::to_string(imageIndex) + ".f32";
        {
            std::ofstream dump(dumpPath, std::ios::binary | std::ios::trunc);
            dump.write(reinterpret_cast<const char*>(values), static_cast<std::streamsize>(dumpSize));
        }
        Logger::warn("depth copy dump written to " + dumpPath);

        logicalDevice->vkd.UnmapMemory(logicalDevice->device, stagingMemory);
        logicalDevice->vkd.FreeCommandBuffers(logicalDevice->device, logicalDevice->commandPool, 1, &commandBuffer);
        logicalDevice->vkd.DestroyBuffer(logicalDevice->device, stagingBuffer, nullptr);
        logicalDevice->vkd.FreeMemory(logicalDevice->device, stagingMemory, nullptr);
    }
#else
    static inline void maybeDumpDepthResolveImage(LogicalDevice*, LogicalSwapchain*, uint32_t, VkQueue) {}
#endif

    static void logNvQueueCheckpointData(LogicalDevice* logicalDevice, VkQueue queue, const char* context)
    {
        if (!logicalDevice || !logicalDevice->supportsNvDiagnosticCheckpoints)
            return;

        if (logicalDevice->vkd.GetQueueCheckpointData2NV)
        {
            uint32_t checkpointCount = 0;
            logicalDevice->vkd.GetQueueCheckpointData2NV(queue, &checkpointCount, nullptr);
            if (checkpointCount == 0)
            {
                Logger::warn(std::string(context) + ": no VK_NV checkpoint data available");
                return;
            }

            std::vector<VkCheckpointData2NV> checkpoints(checkpointCount);
            for (auto& checkpoint : checkpoints)
            {
                checkpoint = {};
                checkpoint.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV;
            }
            logicalDevice->vkd.GetQueueCheckpointData2NV(queue, &checkpointCount, checkpoints.data());

            for (uint32_t i = 0; i < checkpointCount; ++i)
            {
                const auto& checkpoint = checkpoints[i];
                Logger::warn(
                    std::string(context) + ": checkpoint[" + std::to_string(i) +
                    "] stage=0x" + formatHexU64(static_cast<uint64_t>(checkpoint.stage)) +
                    " marker=0x" + formatHexU64(reinterpret_cast<uint64_t>(checkpoint.pCheckpointMarker)));
            }
            return;
        }

        if (logicalDevice->vkd.GetQueueCheckpointDataNV)
        {
            uint32_t checkpointCount = 0;
            logicalDevice->vkd.GetQueueCheckpointDataNV(queue, &checkpointCount, nullptr);
            if (checkpointCount == 0)
            {
                Logger::warn(std::string(context) + ": no VK_NV checkpoint data available");
                return;
            }

            std::vector<VkCheckpointDataNV> checkpoints(checkpointCount);
            for (auto& checkpoint : checkpoints)
            {
                checkpoint = {};
                checkpoint.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
            }
            logicalDevice->vkd.GetQueueCheckpointDataNV(queue, &checkpointCount, checkpoints.data());

            for (uint32_t i = 0; i < checkpointCount; ++i)
            {
                const auto& checkpoint = checkpoints[i];
                Logger::warn(
                    std::string(context) + ": checkpoint[" + std::to_string(i) +
                    "] stage=0x" + formatHexU64(static_cast<uint64_t>(checkpoint.stage)) +
                    " marker=0x" + formatHexU64(reinterpret_cast<uint64_t>(checkpoint.pCheckpointMarker)));
            }
        }
    }

    static void logDeviceFaultInfo(LogicalDevice* logicalDevice, const char* context)
    {
        if (!logicalDevice || !logicalDevice->supportsDeviceFaultExt || !logicalDevice->vkd.GetDeviceFaultInfoEXT)
            return;

        VkDeviceFaultCountsEXT faultCounts = {};
        faultCounts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
        VkResult countsResult = logicalDevice->vkd.GetDeviceFaultInfoEXT(logicalDevice->device, &faultCounts, nullptr);
        if (countsResult != VK_SUCCESS && countsResult != VK_INCOMPLETE)
        {
            Logger::warn(std::string(context) + ": vkGetDeviceFaultInfoEXT(counts) failed: " + std::to_string(countsResult));
            return;
        }

        std::vector<VkDeviceFaultAddressInfoEXT> addressInfos(faultCounts.addressInfoCount);
        std::vector<VkDeviceFaultVendorInfoEXT> vendorInfos(faultCounts.vendorInfoCount);
        std::vector<uint8_t> vendorBinary(faultCounts.vendorBinarySize);

        VkDeviceFaultInfoEXT faultInfo = {};
        faultInfo.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
        faultInfo.pAddressInfos = addressInfos.empty() ? nullptr : addressInfos.data();
        faultInfo.pVendorInfos = vendorInfos.empty() ? nullptr : vendorInfos.data();
        faultInfo.pVendorBinaryData = vendorBinary.empty() ? nullptr : vendorBinary.data();

        VkResult infoResult = logicalDevice->vkd.GetDeviceFaultInfoEXT(logicalDevice->device, &faultCounts, &faultInfo);
        if (infoResult != VK_SUCCESS && infoResult != VK_INCOMPLETE)
        {
            Logger::warn(std::string(context) + ": vkGetDeviceFaultInfoEXT(info) failed: " + std::to_string(infoResult));
            return;
        }

        Logger::err(
            std::string(context) + ": device fault description=\"" + faultInfo.description +
            "\", addresses=" + std::to_string(faultCounts.addressInfoCount) +
            ", vendorInfos=" + std::to_string(faultCounts.vendorInfoCount) +
            ", vendorBinaryBytes=" + std::to_string(faultCounts.vendorBinarySize));

        for (uint32_t i = 0; i < faultCounts.addressInfoCount; ++i)
        {
            const auto& address = addressInfos[i];
            Logger::warn(
                std::string(context) + ": faultAddress[" + std::to_string(i) +
                "] type=" + std::to_string(address.addressType) +
                " reported=0x" + formatHexU64(address.reportedAddress) +
                " precision=0x" + formatHexU64(address.addressPrecision));
        }
    }

    void reportDeviceLostDiagnostics(LogicalDevice* logicalDevice, VkQueue queue, const char* context, VkResult result)
    {
        if (result != VK_ERROR_DEVICE_LOST || !logicalDevice || !logicalDevice->gpuCrashDiagnosticsEnabled)
            return;

        Logger::err(std::string(context) + ": VK_ERROR_DEVICE_LOST (gathering diagnostics)");
        logNvQueueCheckpointData(logicalDevice, queue, context);
        logDeviceFaultInfo(logicalDevice, context);
    }

    // Signal-safe crash recovery for SIGFPE/SIGABRT/SIGSEGV from the embedded
    // reshadefx compiler. C++ try/catch cannot catch these, so we install a
    // siglongjmp trampoline that the compiler path arms.
    //
    // The trampoline is thread-local: only the thread that armed it can be
    // recovered. Signals delivered to other threads fall through to the
    // default handler (which terminates the process — that's correct, since
    // we have no recovery context for them).
    static thread_local sigjmp_buf signalJmpBuf;
    static thread_local volatile sig_atomic_t signalJmpActive = 0;
    static thread_local volatile sig_atomic_t caughtSignal = 0;

    static void crashSignalHandler(int sig)
    {
        if (signalJmpActive)
        {
            caughtSignal = sig;
            siglongjmp(signalJmpBuf, 1);
        }
        // Async-signal-safe backtrace dump, then default disposition.
        const char* sigName = (sig == SIGFPE) ? "SIGFPE"
                            : (sig == SIGABRT) ? "SIGABRT"
                            : (sig == SIGSEGV) ? "SIGSEGV" : "SIGNAL";
        constexpr const char* prefix = "\nVKIntox: caught ";
        (void)write(2, prefix, strlen(prefix));
        (void)write(2, sigName, strlen(sigName));
        constexpr const char* mid = " — backtrace:\n";
        (void)write(2, mid, strlen(mid));
        void* frames[32];
        int count = backtrace(frames, 32);
        backtrace_symbols_fd(frames, count, 2);
        (void)write(2, "\n", 1);
        // Restore default handler and re-raise so the process dies cleanly
        // (and produces a core dump if the user has cores enabled).
        struct sigaction sa = {};
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, nullptr);
        raise(sig);
    }

    static std::once_flag g_crashHandlerOnce;
    static void installCrashHandlers()
    {
        std::call_once(g_crashHandlerOnce, []() {
            struct sigaction sa = {};
            sa.sa_handler = crashSignalHandler;
            sa.sa_flags = SA_RESETHAND;  // Auto-reset on delivery so a second
                                          // signal terminates the process.
            sigemptyset(&sa.sa_mask);
            sigaction(SIGFPE, &sa, nullptr);
            sigaction(SIGABRT, &sa, nullptr);
            sigaction(SIGSEGV, &sa, nullptr);
        });
    }

    // Push a fatal-error toast onto the device's overlay and switch the layer
    // into pass-through mode. The game keeps rendering; effects are disabled
    // until the device is destroyed (i.e. until the user restarts the game).
    void panicLayer(LogicalDevice* logicalDevice, const std::string& reason)
    {
        Logger::err("VKIntox panic: " + reason);
        if (logicalDevice)
        {
            // A panic means we are about to stop touching Vulkan, so anything still
            // queued has to go now. The queue may never be flushed again on this
            // path, and leaving driver objects alive after a device-lost is both a
            // leak and, on some drivers, a fault of its own.
            DeferredDestroyQueue::instance().flush();

            if (reason.find("Device lost") != std::string::npos
                || reason.find("device lost") != std::string::npos
                || reason.find("VK_ERROR_DEVICE_LOST") != std::string::npos)
            {
                std::lock_guard<std::mutex> lossLock(deviceLossLock);
                deviceLostDevices.insert(logicalDevice);
            }

            logicalDevice->softDisabled.store(true, std::memory_order_release);
            if (logicalDevice->imguiOverlay)
                logicalDevice->imguiOverlay->pushToast(
                    LogLevel::Error,
                    "VKIntox disabled itself to keep the game alive.\nReason: " + reason +
                    "\nEffects are off until you restart the game.");
        }
    }

    // Helper for key press with debounce - returns true on key-down edge

    // Detected game info (set once at init, used by overlay for profiles)
    static std::string detectedGameName;
    static std::string activeProfileName;
    static std::string activeProfilePath;
    static std::string activeShaderProfilePath;

    void applyShaderProfile(Config* config, const std::string& path)
    {
        if (!config)
            return;
        if (!path.empty())
        {
            const auto profile = ConfigSerializer::loadShaderProfileData(path);
            const auto sidecar = ConfigSerializer::loadShaderProfileData(
                ConfigSerializer::getShaderProfileSidecarPath(path));

            auto join = [](const std::vector<std::string>& values) {
                std::string result;
                for (const auto& value : values)
                {
                    if (!result.empty()) result += ':';
                    result += value;
                }
                return result;
            };

            // an owned profile's sidecar is the complete stack: register any
            // definitions it misses before they are collected, so the game
            // config's stale effect list never merges in and a shader the
            // config forgot resolves by filename via the include paths
            if (sidecar.owned)
                for (const auto& instance : sidecar.instances)
                    if (!config->hasOption(instance.name))
                        config->setOption(instance.name, instance.type);

            auto definitions = config->getEffectDefinitions();
            if (baseConfig)
            {
                const auto baseDefinitions = baseConfig->getEffectDefinitions();
                for (const auto& [name, effectPath] : baseDefinitions)
                    definitions.emplace(name, effectPath);
            }
            std::map<std::string, std::vector<std::string>> namesByFile;
            for (const auto& [name, effectPath] : definitions)
            {
                std::string filename = std::filesystem::path(effectPath).filename().string();
                std::transform(filename.begin(), filename.end(), filename.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                namesByFile[filename].push_back(name);
            }
            auto applyParams = [&](const std::vector<ConfigParam>& params) {
            for (const auto& param : params)
            {
                std::vector<std::string> effectNames;
                std::string section = param.effectName;
                std::transform(section.begin(), section.end(), section.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                auto it = namesByFile.find(section);
                if (it == namesByFile.end())
                    it = namesByFile.find(std::filesystem::path(section).stem().string());
                if (it != namesByFile.end())
                    effectNames = it->second;
                else if (!param.effectName.empty())
                {
                    const std::filesystem::path sectionPath(param.effectName);
                    effectNames.push_back(sectionPath.extension() == ".fx"
                                              ? sectionPath.stem().string()
                                              : param.effectName);
                }
                if (!param.paramName.empty() && param.paramName.front() == '@')
                {
                    const std::string macroName = param.paramName.substr(1);
                    if (effectNames.empty())
                    {
                        for (const auto& [name, effectPath] : definitions)
                            config->setOption(name + "@" + macroName, param.value);
                    }
                    else
                    {
                        for (const auto& effectName : effectNames)
                            config->setOption(effectName + "@" + macroName, param.value);
                    }
                    continue;
                }
                for (const auto& effectName : effectNames)
                    config->setOption(effectName + "." + param.paramName, param.value);
            }
            };
            applyParams(profile.params);
            if (sidecar.owned || (!profile.hasEffectList && profile.hasTechniques))
                applyParams(sidecar.params);

            if (sidecar.owned)
            {
                const size_t maxEffects = static_cast<size_t>(settingsManager.getMaxEffects());
                std::vector<std::string> effects, disabled;
                for (const auto& instance : sidecar.instances)
                {
                    if (effects.size() >= maxEffects)
                        break;
                    // skip the removed built-ins: they have no .fx, so a legacy
                    // reference drops here and auto-save rewrites it away. an
                    // installed shader of the same name still resolves.
                    if (EffectRegistry::resolveEffectPath(instance.name, config).empty())
                        continue;
                    effects.push_back(instance.name);
                    if (!instance.enabled)
                        disabled.push_back(instance.name);
                }
                config->setOption("effects", join(effects));
                config->setOption("disabledEffects", join(disabled));
            }
            else if (profile.hasEffectList)
            {
                std::vector<std::string> effects, disabled;
                std::set<std::string> disabledSet(profile.disabledEffects.begin(), profile.disabledEffects.end());
                for (const auto& name : profile.effects)
                {
                    if (EffectRegistry::resolveEffectPath(name, config).empty())
                        continue;
                    effects.push_back(name);
                    if (disabledSet.count(name))
                        disabled.push_back(name);
                }
                config->setOption("effects", join(effects));
                config->setOption("disabledEffects", join(disabled));
            }
            else if (!profile.techniques.empty() || !profile.techniqueSorting.empty())
            {
                // TechniqueSorting is an ordering hint for all techniques in a
                // preset. Techniques is the actual enabled list. Expanding the
                // sorting list (or matching only by filename) imports every
                // technique from a shader pack and can create hundreds of
                // disabled instances that make the overlay unusable.
                std::vector<std::string> effects, disabled;
                const auto& sortedTechniques = profile.techniqueSorting.empty()
                    ? profile.techniques
                    : profile.techniqueSorting;
                std::set<std::string> enabledTechniques;
                for (const auto& technique : profile.techniques)
                {
                    enabledTechniques.insert(technique);
                }
                const bool hasExplicitEnabledTechniques = profile.hasTechniques;
                std::set<std::string> addedEffects;
                std::set<std::string> matchedConfiguredEffects;
                const size_t maxEffects = static_cast<size_t>(settingsManager.getMaxEffects());
                for (const auto& technique : sortedTechniques)
                {
                    if (effects.size() >= maxEffects)
                        break;
                    // TechniqueSorting lists every technique a preset knows;
                    // Techniques is the enabled set. expanding sorting imports
                    // hundreds of disabled instances and buries the UI.
                    if (hasExplicitEnabledTechniques && !enabledTechniques.count(technique))
                        continue;
                    const size_t separator = technique.rfind('@');
                    if (separator == std::string::npos || separator + 1 >= technique.size())
                        continue;

                    const std::string filename = technique.substr(separator + 1);
                    const std::string techniqueName = technique.substr(0, separator);
                    std::string normalizedFilename = filename;
                    std::transform(normalizedFilename.begin(), normalizedFilename.end(), normalizedFilename.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    auto configuredNames = namesByFile.find(normalizedFilename);
                    if (configuredNames == namesByFile.end())
                        configuredNames = namesByFile.find(std::filesystem::path(normalizedFilename).stem().string());
                    std::string effectName;

                    // Resolve imported preset techniques to an existing
                    // VKIntox instance by exact technique name. Fall back to
                    // the configured shader filename only when the preset
                    // uses a name that does not correspond to a configured
                    // instance; never expand one file to every instance.
                    if (config->hasOption(techniqueName))
                        effectName = techniqueName;
                    else if (configuredNames != namesByFile.end())
                    {
                        const auto candidate = std::find_if(configuredNames->second.begin(), configuredNames->second.end(),
                            [&](const std::string& name) {
                                return !matchedConfiguredEffects.count(name) && name == techniqueName;
                            });
                        if (candidate != configuredNames->second.end())
                            effectName = *candidate;
                        else if (configuredNames->second.size() == 1 &&
                                 !matchedConfiguredEffects.count(configuredNames->second.front()))
                            effectName = configuredNames->second.front();
                    }
                    // not a configured instance: create one for the installed
                    // shader, so importing isn't limited to effects the game
                    // config already listed. never fall back to the bare
                    // technique name, which resolves to nothing and ghosts.
                    if (effectName.empty())
                    {
                        const std::string stem = std::filesystem::path(filename).stem().string();
                        const std::string resolved = EffectRegistry::resolveEffectPath(stem, config);
                        if (!resolved.empty())
                        {
                            effectName = stem;
                            config->setOption(stem, resolved);
                        }
                    }
                    if (effectName.empty())
                    {
                        Logger::debug("preset technique '" + techniqueName + "' from " + filename
                                      + " matches no installed effect; skipping");
                        continue;
                    }

                    if (!addedEffects.insert(effectName).second)
                        continue;
                    matchedConfiguredEffects.insert(effectName);
                    effects.push_back(effectName);
                }

                config->setOption("effects", join(effects));
                config->setOption("disabledEffects", join(disabled));
            }
        }
        activeShaderProfilePath = path;
    }

    // Initialize configs: base (VKIntox.conf) + current (from game profile / env / default)
    void initConfigs()
    {
        std::call_once(initConfigsOnceFlag, []()
        {
            // Ensure config directory exists for later saves
            {
                std::string baseDir = ConfigSerializer::getBaseConfigDir();
                if (!baseDir.empty())
                    mkdir(baseDir.c_str(), 0755);
            }

            // keep the config dir's version file in step with the built library
            ConfigSerializer::ensureVersionFile();

            // move shaders out of the old reshade/packages/ tree before anything
            // scans for them
            ConfigSerializer::migrateLegacyReshadePackages();

            // Initialize settings manager (single source of truth for settings)
            settingsManager.initialize();

            // Load base config (VKIntox.conf) - used for paths, effect definitions
            baseConfig = std::make_shared<Config>();

            // Detect the game executable
            detectedGameName = ConfigSerializer::detectGameName();

            // Determine current config path (priority order):
            // 1. VKINTOX_CONFIG_FILE env var (explicit override)
            // 2. Per-game profile (auto-created if needed)
            // 3. Legacy default_config file
            // 4. Base VKIntox.conf
            std::string currentConfigPath;

            const char* envConfig = std::getenv("VKINTOX_CONFIG_FILE");
            if (envConfig && *envConfig)
            {
                currentConfigPath = envConfig;
                Logger::info("config from env: " + currentConfigPath);
            }
            else if (!detectedGameName.empty())
            {
                // Auto-create profile for this game if needed, then load it
                activeProfileName = "default";
                activeProfilePath = ConfigSerializer::getProfilePath(detectedGameName);

                // Ensure the profile file exists
                struct stat st;
                if (stat(activeProfilePath.c_str(), &st) != 0)
                {
                    // Profile doesn't exist yet — create it
                    activeProfilePath = ConfigSerializer::ensureGameProfile(detectedGameName);
                }

                if (!activeProfilePath.empty())
                {
                    if (ConfigSerializer::listShaderProfilesForGame(detectedGameName).empty())
                        ConfigSerializer::createShaderProfile(detectedGameName, "default");
                    const auto shaderProfiles = ConfigSerializer::listShaderProfilesForGame(detectedGameName);
                    std::string lastShaderProfile = ConfigSerializer::getLastShaderProfile(detectedGameName);
                    if (std::find(shaderProfiles.begin(), shaderProfiles.end(), lastShaderProfile) == shaderProfiles.end())
                        lastShaderProfile = "default";
                    activeShaderProfilePath = ConfigSerializer::getShaderProfilePath(detectedGameName, lastShaderProfile);
                    currentConfigPath = activeProfilePath;
                    Logger::info("game: " + detectedGameName + " | config: " + activeProfilePath);
                }
            }

            // Fallback: legacy default_config
            if (currentConfigPath.empty())
            {
                std::string defaultName = ConfigSerializer::getDefaultConfig();
                if (!defaultName.empty())
                    currentConfigPath = ConfigSerializer::getConfigsDir() + "/" + defaultName + ".conf";
            }

            // Load current config if specified, otherwise use base
            if (!currentConfigPath.empty())
            {
                std::ifstream file(currentConfigPath);
                if (file.good())
                {
                    config = std::make_shared<Config>(currentConfigPath);
                    config->setFallback(baseConfig.get());
                    if (!detectedGameName.empty())
                    {
                        applyShaderProfile(config.get(), activeShaderProfilePath);
                    }
                    Logger::info("current config: " + currentConfigPath);
                }
                else
                {
                    config = baseConfig;
                }
            }
            else
            {
                config = baseConfig;
            }

            // Initialize effect registry with current config
            effectRegistry.initialize(config.get());
        });
    }

    // Switch to a new config (called from overlay)
    void switchConfig(const std::string& configPath, const std::string& shaderPath = "")
    {
        Logger::info("switching to config: " + configPath);

        // Create new config from file (starts with no overrides)
        config = std::make_shared<Config>(configPath);
        config->setFallback(baseConfig.get());
        applyShaderProfile(config.get(), shaderPath);

        // Also clear any overrides on the base config to avoid stale values
        if (baseConfig)
            baseConfig->clearOverrides();

        // Re-initialize registry with new config
        effectRegistry.initialize(config.get());
        cachedParams.dirty = true;

        Logger::info("switched to config: " + configPath);
    }

    // Helper function to get available effects separated by source (uses cache)
    void getAvailableEffects(Config* config,
                             std::vector<std::string>& currentConfigEffects,
                             std::vector<std::string>& defaultConfigEffects,
                             std::map<std::string, std::string>& effectPaths)
    {
        // Use cache if available and config hasn't changed
        if (cachedEffects.initialized && cachedEffects.configPath == config->getConfigFilePath())
        {
            currentConfigEffects = cachedEffects.currentConfigEffects;
            defaultConfigEffects = cachedEffects.defaultConfigEffects;
            effectPaths = cachedEffects.effectPaths;
            return;
        }

        currentConfigEffects.clear();
        defaultConfigEffects.clear();
        effectPaths.clear();

        // Collect all known effect names (to avoid duplicates)
        std::set<std::string> knownEffects;

        // Get effect definitions from current config
        auto configEffects = config->getEffectDefinitions();
        for (const auto& [name, path] : configEffects)
        {
            currentConfigEffects.push_back(name);
            effectPaths[name] = path;
            knownEffects.insert(name);
        }

        // Also load effect definitions from the base config file (VKIntox.conf)
        if (baseConfig && baseConfig->getConfigFilePath() != config->getConfigFilePath())
        {
            auto defaultEffects = baseConfig->getEffectDefinitions();
            for (const auto& [name, path] : defaultEffects)
            {
                if (knownEffects.find(name) == knownEffects.end())
                {
                    defaultConfigEffects.push_back(name);
                    effectPaths[name] = path;
                    knownEffects.insert(name);
                }
            }
        }

        // Helper: scan a list of directories for .fx files and add discovered effects
        auto scanDirsForFx = [&](const std::vector<std::string>& dirs)
        {
            for (const auto& dir : dirs)
            {
                try
                {
                    for (const auto& entry : std::filesystem::recursive_directory_iterator(
                             dir, std::filesystem::directory_options::skip_permission_denied))
                    {
                        if (!entry.is_regular_file())
                            continue;

                        std::string ext = entry.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext != ".fx")
                            continue;

                        std::string effectName = entry.path().stem().string();

                        if (knownEffects.find(effectName) != knownEffects.end())
                            continue;

                        defaultConfigEffects.push_back(effectName);
                        effectPaths[effectName] = entry.path().string();
                        knownEffects.insert(effectName);
                    }
                }
                catch (const std::filesystem::filesystem_error& e)
                {
                    Logger::warn("failed to scan shader path " + dir + ": " + std::string(e.what()));
                }
            }
        };

        // Collect ALL directories to scan for .fx files
        std::set<std::string> allScanDirs;  // deduplicate

        // 1. reshadeIncludePath from config (colon-separated)
        //    This is the most common way users configure shader paths,
        //    but getAvailableEffects was previously not scanning it at all.
        std::string includePath = config->getOption<std::string>("reshadeIncludePath", "");
        if (!includePath.empty())
        {
            std::stringstream ss(includePath);
            std::string dir;
            while (std::getline(ss, dir, ':'))
            {
                if (!dir.empty())
                    allScanDirs.insert(dir);
            }
        }
        // Also check the base config's reshadeIncludePath (via fallback)
        if (baseConfig && baseConfig->getConfigFilePath() != config->getConfigFilePath())
        {
            std::string baseInclude = baseConfig->getOption<std::string>("reshadeIncludePath", "");
            if (!baseInclude.empty())
            {
                std::stringstream ss(baseInclude);
                std::string dir;
                while (std::getline(ss, dir, ':'))
                {
                    if (!dir.empty())
                        allScanDirs.insert(dir);
                }
            }
        }

        // 2. Shader manager paths
        ShaderManagerConfig shaderMgrConfig = ConfigSerializer::loadShaderManagerConfig();

        // 2a. Discovered shader paths (previously the only source scanned)
        for (const auto& p : shaderMgrConfig.discoveredShaderPaths)
            allScanDirs.insert(p);

        // 2b. Parent directories — re-scan for Shaders/ subdirectories
        //     (they may have been added after the last shader_manager.conf save,
        //      or the initial scan may have missed them)
        for (const auto& parentDir : shaderMgrConfig.parentDirectories)
        {
            try
            {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(
                         parentDir, std::filesystem::directory_options::skip_permission_denied))
                {
                    if (!entry.is_directory())
                        continue;

                    std::string dirName = entry.path().filename().string();
                    // Case-insensitive comparison for "Shaders"
                    std::transform(dirName.begin(), dirName.end(), dirName.begin(), ::tolower);
                    if (dirName == "shaders")
                        allScanDirs.insert(entry.path().string());
                }
            }
            catch (const std::filesystem::filesystem_error&) {}

            // Also scan the parent dir itself — users often place .fx files
            // directly in the parent directory without a Shaders/ subdirectory.
            allScanDirs.insert(parentDir);
        }

        // Scan all collected directories for .fx files
        scanDirsForFx({allScanDirs.begin(), allScanDirs.end()});

        // Sort discovered effects alphabetically
        std::sort(defaultConfigEffects.begin(), defaultConfigEffects.end());

        // Update cache
        cachedEffects.currentConfigEffects = currentConfigEffects;
        cachedEffects.defaultConfigEffects = defaultConfigEffects;
        cachedEffects.effectPaths = effectPaths;
        cachedEffects.configPath = config->getConfigFilePath();
        cachedEffects.initialized = true;
    }

    // Helper function to create effects for a swapchain
    // This centralizes the effect creation logic used by both initial swapchain setup and hot-reload
    void createEffectsForSwapchain(
        LogicalSwapchain* logicalSwapchain,
        LogicalDevice* logicalDevice,
        Config* config,
        const std::vector<std::string>& effectStrings,
        bool checkEnabledState = true)
    {
        const bool useMutableFormat = logicalSwapchain->useMutableFormat;

        if (logicalSwapchain->imageCount == 0)
        {
            Logger::err("Cannot create effects for swapchain with imageCount=0");
            return;
        }

        const size_t requiredSlots = effectStrings.empty()
            ? 1
            : effectStrings.size() + (useMutableFormat ? 0u : 1u);
        const size_t requiredFakeImages = static_cast<size_t>(logicalSwapchain->imageCount) * requiredSlots;
        if (logicalSwapchain->fakeImages.size() < requiredFakeImages)
        {
            Logger::err("Insufficient fake images for effect chain: have "
                        + std::to_string(logicalSwapchain->fakeImages.size()) + ", need "
                        + std::to_string(requiredFakeImages));
            return;
        }

        // If no effects, add pass-through so rendering still works
        if (effectStrings.empty())
        {
            std::vector<VkImage> firstImages(logicalSwapchain->fakeImages.begin(),
                                             logicalSwapchain->fakeImages.begin() + logicalSwapchain->imageCount);
            logicalSwapchain->effects.push_back(std::make_shared<TransferEffect>(
                logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent,
                firstImages, logicalSwapchain->images, config));
            return;
        }

        for (uint32_t i = 0; i < effectStrings.size(); i++)
        {
            Logger::debug("creating effect " + std::to_string(i) + ": " + effectStrings[i]);

            // Calculate input images for this effect
            std::vector<VkImage> firstImages(logicalSwapchain->fakeImages.begin() + logicalSwapchain->imageCount * i,
                                             logicalSwapchain->fakeImages.begin() + logicalSwapchain->imageCount * (i + 1));

            // Calculate output images - last effect writes to swapchain or final fake images
            std::vector<VkImage> secondImages;
            if (i == effectStrings.size() - 1)
            {
                secondImages = useMutableFormat
                    ? logicalSwapchain->images
                    : std::vector<VkImage>(logicalSwapchain->fakeImages.end() - logicalSwapchain->imageCount,
                                           logicalSwapchain->fakeImages.end());
            }
            else
            {
                secondImages = std::vector<VkImage>(logicalSwapchain->fakeImages.begin() + logicalSwapchain->imageCount * (i + 1),
                                                    logicalSwapchain->fakeImages.begin() + logicalSwapchain->imageCount * (i + 2));
            }

            // Check if effect should be skipped (disabled or failed)
            bool effectFailed = effectRegistry.hasEffectFailed(effectStrings[i]);
            bool effectDisabled = checkEnabledState && !effectRegistry.isEffectEnabled(effectStrings[i]);

            if (effectFailed || effectDisabled)
            {
                Logger::debug("effect " + std::string(effectFailed ? "failed" : "disabled") + ", using pass-through: " + effectStrings[i]);
                logicalSwapchain->effects.push_back(std::make_shared<TransferEffect>(logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent, firstImages, secondImages, config));
                continue;
            }

            // ReShade effect - wrap in try-catch + signal handler to handle compilation failures gracefully
            // The embedded reshadefx compiler can trigger SIGFPE/SIGABRT in edge cases
            std::string effectPath = effectRegistry.getEffectFilePath(effectStrings[i]);
            auto customDefs = effectRegistry.getPreprocessorDefs(effectStrings[i]);

            installCrashHandlers();
            bool signalCrash = false;
            if (sigsetjmp(signalJmpBuf, 1) != 0)
            {
                // Returned here from signal handler (SIGFPE/SIGABRT/SIGSEGV).
                // The C++ stack was unwound by siglongjmp, so any RAII
                // guards above us in the createEffectsForSwapchain frame
                // (including the scoped_lock on globalLock) were NOT
                // destroyed. We must NOT touch globalLock-protected state
                // from here — just push a passthrough effect and continue.
                signalJmpActive = 0;
                signalCrash = true;
                std::string sigName = (caughtSignal == SIGFPE) ? "SIGFPE"
                                    : (caughtSignal == SIGABRT) ? "SIGABRT"
                                    : (caughtSignal == SIGSEGV) ? "SIGSEGV" : "SIGNAL";
                Logger::err("Caught " + sigName + " creating ReshadeEffect " + effectStrings[i]);
                effectRegistry.setEffectError(effectStrings[i], sigName + " during shader compilation");
                logicalSwapchain->effects.push_back(std::make_shared<TransferEffect>(logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent, firstImages, secondImages, config));
                // Soft-disable: reshadefx native crash means we can't
                // trust the compiler. Don't risk another one.
                panicLayer(logicalDevice, std::string("reshadefx native crash (") + sigName
                           + ") while compiling " + effectStrings[i]);
            }

            if (!signalCrash)
            {
                signalJmpActive = 1;
                try
                {
                    auto reshadeEffect = std::make_shared<ReshadeEffect>(
                        logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent,
                        firstImages, secondImages, &effectRegistry, effectStrings[i], effectPath, customDefs);
                    logicalSwapchain->effects.push_back(reshadeEffect);
                    if (reshadeEffect->getOutputWrites() == 0)
                    {
                        Logger::debug("deterministic forwarding for zero-output-write effect: " + effectStrings[i]);
                        logicalSwapchain->effects.push_back(std::make_shared<TransferEffect>(logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent,
                                               firstImages, secondImages, config));
                    }
                }
                catch (const std::exception& e)
                {
                    Logger::err("Failed to create ReshadeEffect " + effectStrings[i] + ": " + e.what());
                    effectRegistry.setEffectError(effectStrings[i], e.what());
                    logicalSwapchain->effects.push_back(std::make_shared<TransferEffect>(logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent, firstImages, secondImages, config));
                }
                signalJmpActive = 0;
            }
        }

        // If device doesn't support mutable format, add final transfer to swapchain
        if (!useMutableFormat)
        {
            logicalSwapchain->effects.push_back(std::make_shared<TransferEffect>(
                logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent,
                std::vector<VkImage>(logicalSwapchain->fakeImages.end() - logicalSwapchain->imageCount, logicalSwapchain->fakeImages.end()),
                logicalSwapchain->images, config));
        }
    }

    // Helper function to reload effects for a swapchain (for hot-reload)
    void reloadEffectsForSwapchain(LogicalSwapchain* logicalSwapchain, Config* config,
                                   const std::vector<std::string>& activeEffects = {})
    {
        LogicalDevice* logicalDevice = logicalSwapchain->logicalDevice;

        // Wait for GPU to finish
        logicalDevice->vkd.QueueWaitIdle(logicalDevice->queue);

        // Clear effects (command buffers will be freed by reallocateCommandBuffers)
        logicalSwapchain->effects.clear();
        logicalSwapchain->defaultTransfer.reset();

        // Use provided active effects list directly - no fallback to config
        // Registry is the single source of truth (initialized at first swapchain creation)
        std::vector<std::string> effectStrings = activeEffects;

        // Check if we have enough fake images for the effects
        // Fake images are allocated at swapchain creation based on maxEffectSlots
        if (effectStrings.size() > logicalSwapchain->maxEffectSlots)
        {
            Logger::warn("Cannot add more effects than maxEffectSlots (" +
                        std::to_string(effectStrings.size()) + " > " + std::to_string(logicalSwapchain->maxEffectSlots) +
                        "). Increase maxEffects in config.");
            effectStrings.resize(logicalSwapchain->maxEffectSlots);
        }

        Logger::info("reloading " + std::to_string(effectStrings.size()) + " effects");

        // Create effects using centralized helper
        createEffectsForSwapchain(logicalSwapchain, logicalDevice, config, effectStrings, true);

        // Create default transfer effect (needed for no-effect command buffers)
        logicalSwapchain->defaultTransfer = std::make_shared<TransferEffect>(
            logicalDevice,
            logicalSwapchain->format,
            logicalSwapchain->imageExtent,
            std::vector<VkImage>(logicalSwapchain->fakeImages.begin(), logicalSwapchain->fakeImages.begin() + logicalSwapchain->imageCount),
            logicalSwapchain->images,
            config);

        // Free old command buffers and allocate/write new ones
        DepthState depth = getDepthState(logicalDevice);
        reallocateCommandBuffers(logicalDevice, logicalSwapchain, depth);

        Logger::info("effects reloaded successfully");
    }

    // Reload effects for all swapchains belonging to a device
    void reloadAllSwapchains(LogicalDevice* /* logicalDevice */, const std::vector<std::string>& activeEffects)
    {
        for (auto& [_, logicalSwapchain] : swapchainMap)
        {
            if (!logicalSwapchain->fakeImages.empty())
                reloadEffectsForSwapchain(logicalSwapchain.get(), config.get(), activeEffects);
        }
    }

    // Build and update overlay state for rendering
    void updateOverlayState(LogicalDevice* logicalDevice, bool effectsEnabled)
    {
        if (!logicalDevice->imguiOverlay || !logicalDevice->imguiOverlay->isVisible())
            return;

        OverlayState overlayState;

        // No fallback to config - registry is the single source of truth
        // (initialized from config at first swapchain creation)

        getAvailableEffects(config.get(), overlayState.currentConfigEffects,
                            overlayState.defaultConfigEffects, overlayState.effectPaths);
        overlayState.configPath = config->getConfigFilePath();

        // Cache the filename extraction — config path rarely changes
        static std::string cachedConfigPath;
        static std::string cachedConfigName;
        if (overlayState.configPath != cachedConfigPath)
        {
            cachedConfigPath = overlayState.configPath;
            cachedConfigName = std::filesystem::path(cachedConfigPath).filename().string();
        }
        overlayState.configName = cachedConfigName;
        overlayState.effectsEnabled = effectsEnabled;

        // Ensure all selected effects are in the registry
        for (const auto& effectName : logicalDevice->imguiOverlay->getSelectedEffects())
        {
            if (effectRegistry.hasEffect(effectName))
                continue;
            auto pathIt = overlayState.effectPaths.find(effectName);
            std::string effectPath = (pathIt != overlayState.effectPaths.end()) ? pathIt->second : "";
            effectRegistry.ensureEffect(effectName, effectPath);
        }

        // Parameters now read directly from EffectRegistry, no need to pass via state
        logicalDevice->imguiOverlay->updateState(std::move(overlayState));
    }

    // Submit overlay command buffer if visible, returns semaphore to wait on
    VkResult submitOverlayFrame(LogicalDevice* logicalDevice, LogicalSwapchain* swapchain,
                                uint32_t index, VkSemaphore& outSemaphore)
    {
        outSemaphore = swapchain->semaphores[index];  // Default: wait on effects semaphore

        if (!logicalDevice->imguiOverlay)
            return VK_SUCCESS;

        VkCommandBuffer overlayCmd = logicalDevice->imguiOverlay->recordFrame(
            index, swapchain->imageViews[index],
            swapchain->imageExtent.width, swapchain->imageExtent.height);

        if (overlayCmd == VK_NULL_HANDLE)
            return VK_SUCCESS;

        VkPipelineStageFlags overlayWaitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo overlaySubmit = {};
        overlaySubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        overlaySubmit.waitSemaphoreCount = 1;
        overlaySubmit.pWaitSemaphores = &swapchain->semaphores[index];
        overlaySubmit.pWaitDstStageMask = &overlayWaitStage;
        overlaySubmit.commandBufferCount = 1;
        overlaySubmit.pCommandBuffers = &overlayCmd;
        overlaySubmit.signalSemaphoreCount = 1;
        overlaySubmit.pSignalSemaphores = &swapchain->overlaySemaphores[index];

        // Use fence to track command buffer completion (prevents reuse while in flight)
        VkFence overlayFence = logicalDevice->imguiOverlay->getCommandBufferFence(index);
        VkResult vr = logicalDevice->vkd.QueueSubmit(logicalDevice->queue, 1, &overlaySubmit, overlayFence);
        if (vr == VK_SUCCESS)
            outSemaphore = swapchain->overlaySemaphores[index];

        return vr;
    }

    VkResult VKAPI_CALL VKIntox_CreateInstance(const VkInstanceCreateInfo*  pCreateInfo,
                                                const VkAllocationCallbacks* pAllocator,
                                                VkInstance*                  pInstance)
    {
        VkLayerInstanceCreateInfo* layerCreateInfo = (VkLayerInstanceCreateInfo*) pCreateInfo->pNext;

        // step through the chain of pNext until we get to the link info
        while (layerCreateInfo
               && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || layerCreateInfo->function != VK_LAYER_LINK_INFO))
        {
            layerCreateInfo = (VkLayerInstanceCreateInfo*) layerCreateInfo->pNext;
        }

        Logger::trace("vkCreateInstance");

        if (layerCreateInfo == nullptr)
        {
            // No loader instance create info
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        PFN_vkGetInstanceProcAddr gpa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        // move chain on for next layer
        layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

        PFN_vkCreateInstance createFunc = (PFN_vkCreateInstance) gpa(VK_NULL_HANDLE, "vkCreateInstance");

        VkInstanceCreateInfo modifiedCreateInfo = *pCreateInfo;
        VkApplicationInfo    appInfo;
        if (modifiedCreateInfo.pApplicationInfo)
        {
            appInfo = *(modifiedCreateInfo.pApplicationInfo);
            if (appInfo.apiVersion < VK_API_VERSION_1_1)
            {
                appInfo.apiVersion = VK_API_VERSION_1_1;
            }
        }
        else
        {
            appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            appInfo.pNext              = nullptr;
            appInfo.pApplicationName   = nullptr;
            appInfo.applicationVersion = 0;
            appInfo.pEngineName        = nullptr;
            appInfo.engineVersion      = 0;
            appInfo.apiVersion         = VK_API_VERSION_1_1;
        }

        modifiedCreateInfo.pApplicationInfo = &appInfo;
        VkResult ret                        = createFunc(&modifiedCreateInfo, pAllocator, pInstance);

        if (ret != VK_SUCCESS)
        {
            // *pInstance is only valid on VK_SUCCESS, and the maps key on the handle
            Logger::err("vkCreateInstance failed: " + std::to_string(ret));
            return ret;
        }

        // fetch our own dispatch table for the functions we need, into the next layer
        InstanceDispatch dispatchTable;
        fillDispatchTableInstance(*pInstance, gpa, &dispatchTable);

        // store the table by key
        {
            scoped_lock l(globalLock);
            instanceDispatchMap[GetKey(*pInstance)] = dispatchTable;
            instanceMap[GetKey(*pInstance)]         = *pInstance;
            instanceVersionMap[GetKey(*pInstance)]  = modifiedCreateInfo.pApplicationInfo->apiVersion;
        }

        return ret;
    }

    void VKAPI_CALL VKIntox_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
    {
        if (!instance)
            return;

        scoped_lock l(globalLock);

        Logger::trace("vkDestroyInstance");

        auto it = instanceDispatchMap.find(GetKey(instance));
        if (it == instanceDispatchMap.end())
        {
            // Not created through this layer; nothing we can safely do
            Logger::trace("vkDestroyInstance: instance not tracked by this layer, skipping");
            return;
        }

        InstanceDispatch dispatchTable = it->second;

        dispatchTable.DestroyInstance(instance, pAllocator);

        instanceDispatchMap.erase(GetKey(instance));
        instanceMap.erase(GetKey(instance));
        instanceVersionMap.erase(GetKey(instance));
    }

    VkResult VKAPI_CALL VKIntox_CreateDevice(VkPhysicalDevice             physicalDevice,
                                              const VkDeviceCreateInfo*    pCreateInfo,
                                              const VkAllocationCallbacks* pAllocator,
                                              VkDevice*                    pDevice)
    {
        scoped_lock l(globalLock);
        Logger::trace("vkCreateDevice");
        VkLayerDeviceCreateInfo* layerCreateInfo = (VkLayerDeviceCreateInfo*) pCreateInfo->pNext;

        // step through the chain of pNext until we get to the link info
        while (layerCreateInfo
               && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || layerCreateInfo->function != VK_LAYER_LINK_INFO))
        {
            layerCreateInfo = (VkLayerDeviceCreateInfo*) layerCreateInfo->pNext;
        }

        if (layerCreateInfo == nullptr)
        {
            // No loader instance create info
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        PFN_vkGetDeviceProcAddr   gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
        // move chain on for next layer
        layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

        PFN_vkCreateDevice createFunc = (PFN_vkCreateDevice) gipa(VK_NULL_HANDLE, "vkCreateDevice");

        // check and activate extentions
        uint32_t extensionCount = 0;

        std::vector<VkExtensionProperties> extensionProperties;

        {
            auto it = instanceDispatchMap.find(GetKey(physicalDevice));
            if (it != instanceDispatchMap.end())
            {
                VkResult res = it->second.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
                if (res != VK_SUCCESS || extensionCount == 0)
                {
                    Logger::warn("EnumerateDeviceExtensionProperties failed or no extensions");
                }
                else
                {
                    extensionProperties.resize(extensionCount);
                    res = it->second.EnumerateDeviceExtensionProperties(
                        physicalDevice, nullptr, &extensionCount, extensionProperties.data());
                    if (res != VK_SUCCESS)
                        Logger::warn("EnumerateDeviceExtensionProperties second call failed");
                }
            }
        }

        auto hasDeviceExtension = [&](const char* extensionName) {
            for (const VkExtensionProperties& properties : extensionProperties)
            {
                if (std::strcmp(properties.extensionName, extensionName) == 0)
                    return true;
            }
            return false;
        };

        const bool supportsMutableFormat = hasDeviceExtension("VK_KHR_swapchain_mutable_format");
        const bool supportsNvCheckpointExt = hasDeviceExtension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        const bool supportsNvDiagnosticsConfigExt = hasDeviceExtension(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
        const bool supportsDeviceFaultExt = hasDeviceExtension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        const bool gpuCrashDiagnosticsRequested = isGpuCrashDiagEnabled();
        if (supportsMutableFormat)
            Logger::debug("device supports VK_KHR_swapchain_mutable_format");

        bool hasMutableEnvOverride = false;
        bool mutableRequested = getMutableSwapchainEnvOverride(hasMutableEnvOverride, true);

        VkPhysicalDeviceProperties deviceProps;
        instanceDispatchMap[GetKey(physicalDevice)].GetPhysicalDeviceProperties(physicalDevice, &deviceProps);

        VkDeviceCreateInfo       modifiedCreateInfo = *pCreateInfo;
        std::vector<const char*> enabledExtensionNames;
        if (modifiedCreateInfo.enabledExtensionCount)
        {
            enabledExtensionNames = std::vector<const char*>(modifiedCreateInfo.ppEnabledExtensionNames,
                                                             modifiedCreateInfo.ppEnabledExtensionNames + modifiedCreateInfo.enabledExtensionCount);
        }

        if (supportsMutableFormat && mutableRequested)
        {
            Logger::debug("activating mutable_format");
            addUniqueCString(enabledExtensionNames, "VK_KHR_swapchain_mutable_format");
        }
        else if (hasMutableEnvOverride)
        {
            Logger::info(std::string("Mutable swapchain device extension set via VKINTOX_ENABLE_MUTABLE_SWAPCHAIN=") + (mutableRequested ? "1" : "0"));
        }
        if (deviceProps.apiVersion < VK_API_VERSION_1_2 || instanceVersionMap[GetKey(physicalDevice)] < VK_API_VERSION_1_2)
        {
            addUniqueCString(enabledExtensionNames, "VK_KHR_image_format_list");
        }

        bool enableNvCheckpoints = false;
        bool enableNvDiagnosticsConfig = false;
        bool enableDeviceFault = false;
        bool enableDeviceFaultFeature = false;
        VkPhysicalDeviceFaultFeaturesEXT faultFeatureEnable = {};
        VkDeviceDiagnosticsConfigCreateInfoNV diagnosticsConfigInfo = {};
        if (gpuCrashDiagnosticsRequested)
        {
            if (supportsNvCheckpointExt)
            {
                addUniqueCString(enabledExtensionNames, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
                enableNvCheckpoints = true;
            }
            else
            {
                Logger::warn("VKINTOX_GPU_CRASH_DIAGNOSTICS=1 but VK_NV_device_diagnostic_checkpoints is not supported by this device");
            }

            if (supportsNvDiagnosticsConfigExt)
            {
                addUniqueCString(enabledExtensionNames, VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
                enableNvDiagnosticsConfig = true;
            }
            else
            {
                Logger::warn("VKINTOX_GPU_CRASH_DIAGNOSTICS=1 but VK_NV_device_diagnostics_config is not supported by this device");
            }

            if (supportsDeviceFaultExt)
            {
                addUniqueCString(enabledExtensionNames, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
                enableDeviceFault = true;
            }
            else
            {
                Logger::warn("VKINTOX_GPU_CRASH_DIAGNOSTICS=1 but VK_EXT_device_fault is not supported by this device");
            }
        }
        modifiedCreateInfo.ppEnabledExtensionNames = enabledExtensionNames.data();
        modifiedCreateInfo.enabledExtensionCount   = enabledExtensionNames.size();

        // Active needed Features
        VkPhysicalDeviceFeatures deviceFeatures = {};
        if (modifiedCreateInfo.pEnabledFeatures)
        {
            deviceFeatures = *(modifiedCreateInfo.pEnabledFeatures);
        }
        deviceFeatures.shaderImageGatherExtended = VK_TRUE;
        deviceFeatures.shaderStorageImageReadWithoutFormat = VK_TRUE;
        deviceFeatures.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        modifiedCreateInfo.pEnabledFeatures      = &deviceFeatures;

        void* extendedDevicePNext = const_cast<void*>(modifiedCreateInfo.pNext);

        if (enableDeviceFault && !pNextChainContainsSType(modifiedCreateInfo.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT))
        {
            VkPhysicalDeviceFaultFeaturesEXT faultFeatureQuery = {};
            faultFeatureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
            VkPhysicalDeviceFeatures2 featureQuery = {};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &faultFeatureQuery;
            if (instanceDispatchMap[GetKey(physicalDevice)].GetPhysicalDeviceFeatures2)
            {
                instanceDispatchMap[GetKey(physicalDevice)].GetPhysicalDeviceFeatures2(physicalDevice, &featureQuery);
                if (faultFeatureQuery.deviceFault == VK_TRUE)
                {
                    faultFeatureEnable = {};
                    faultFeatureEnable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
                    faultFeatureEnable.deviceFault = VK_TRUE;
                    faultFeatureEnable.pNext = extendedDevicePNext;
                    extendedDevicePNext = &faultFeatureEnable;
                    enableDeviceFaultFeature = true;
                }
                else
                {
                    Logger::warn("VK_EXT_device_fault is present but deviceFault feature is not supported");
                }
            }
            else
            {
                Logger::warn("Cannot query VkPhysicalDeviceFaultFeaturesEXT (vkGetPhysicalDeviceFeatures2 unavailable)");
            }
        }
        else if (enableDeviceFault)
        {
            enableDeviceFaultFeature = true;
        }

        if (enableNvDiagnosticsConfig && !pNextChainContainsSType(modifiedCreateInfo.pNext, VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV))
        {
            diagnosticsConfigInfo = {};
            diagnosticsConfigInfo.sType = VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV;
            diagnosticsConfigInfo.flags =
                VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV
                | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_ERROR_REPORTING_BIT_NV;
            diagnosticsConfigInfo.pNext = extendedDevicePNext;
            extendedDevicePNext = &diagnosticsConfigInfo;
        }

        modifiedCreateInfo.pNext = extendedDevicePNext;

        VkResult ret = createFunc(physicalDevice, &modifiedCreateInfo, pAllocator, pDevice);

        if (ret != VK_SUCCESS)
            return ret;

        auto logicalDevice = std::make_shared<LogicalDevice>();
        logicalDevice->vki                   = instanceDispatchMap[GetKey(physicalDevice)];
        logicalDevice->device                = *pDevice;
        logicalDevice->physicalDevice        = physicalDevice;
        logicalDevice->instance              = instanceMap[GetKey(physicalDevice)];
        logicalDevice->queue                 = VK_NULL_HANDLE;
        logicalDevice->queueFamilyIndex      = 0;
        logicalDevice->commandPool           = VK_NULL_HANDLE;
        logicalDevice->supportsMutableFormat = supportsMutableFormat && mutableRequested;
        logicalDevice->isNvidiaGpu           = (deviceProps.vendorID == 0x10DE);
        logicalDevice->gpuCrashDiagnosticsEnabled = gpuCrashDiagnosticsRequested;
        logicalDevice->supportsNvDiagnosticCheckpoints = enableNvCheckpoints;
        logicalDevice->supportsNvDiagnosticsConfig = enableNvDiagnosticsConfig;
        logicalDevice->supportsDeviceFaultExt = enableDeviceFault && enableDeviceFaultFeature;

        fillDispatchTableDevice(*pDevice, gdpa, &logicalDevice->vkd);

        // Query supported depth resolve modes once (VK_KHR_depth_resolve_mode,
        // core since 1.2). Drives the MSAA depth resolve path selection and the
        // Advanced UI mode selector. Every implementation must support at least
        // SAMPLE_ZERO, so this never stays 0 on a conformant driver.
        {
            VkPhysicalDeviceDepthStencilResolveProperties resolveProps = {};
            resolveProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES;
            // the core PCIE struct is missing from our vulkan headers; VK_EXT_pci_bus_info
            // has the same layout and sType
            VkPhysicalDevicePCIBusInfoPropertiesEXT pcieProps = {};
            pcieProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
            VkPhysicalDeviceDriverProperties driverProps = {};
            driverProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            VkPhysicalDeviceProperties2 props2 = {};
            props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            props2.pNext = &resolveProps;
            resolveProps.pNext = &pcieProps;
            pcieProps.pNext = &driverProps;
            if (instanceDispatchMap[GetKey(physicalDevice)].GetPhysicalDeviceProperties2)
            {
                instanceDispatchMap[GetKey(physicalDevice)].GetPhysicalDeviceProperties2(physicalDevice, &props2);
                logicalDevice->supportedDepthResolveModes = resolveProps.supportedDepthResolveModes;
                if (logicalDevice->supportedDepthResolveModes == 0)
                    logicalDevice->supportedDepthResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;

                // identity + driver string, for the Diagnostics GPU card
                logicalDevice->gpuName      = props2.properties.deviceName;
                logicalDevice->gpuApiVersion = props2.properties.apiVersion;
                logicalDevice->gpuVendorId   = props2.properties.vendorID;
                if (driverProps.driverName[0] || driverProps.driverInfo[0])
                {
                    std::string info = driverProps.driverName;
                    if (driverProps.driverInfo[0])
                    {
                        if (!info.empty()) info += " ";
                        info += driverProps.driverInfo;
                    }
                    logicalDevice->gpuDriverInfo = info;
                }
                if (pcieProps.pciDomain != 0 || pcieProps.pciBus != 0 || pcieProps.pciDevice != 0 || pcieProps.pciFunction != 0)
                {
                    char slot[32] = {};
                    snprintf(slot, sizeof(slot), "%04x:%02x:%02x.%x",
                             pcieProps.pciDomain, pcieProps.pciBus, pcieProps.pciDevice, pcieProps.pciFunction);
                    logicalDevice->gpuPciSlot = slot;
                }
            }
            else
            {
                Logger::warn("vkGetPhysicalDeviceProperties2 unavailable; assuming SAMPLE_ZERO depth resolve only");
                logicalDevice->supportedDepthResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            }
            Logger::debug("supported depth resolve modes: " + std::to_string(logicalDevice->supportedDepthResolveModes));
        }

        if (logicalDevice->gpuCrashDiagnosticsEnabled)
        {
            if (logicalDevice->supportsNvDiagnosticCheckpoints
                && !(logicalDevice->vkd.GetQueueCheckpointDataNV || logicalDevice->vkd.GetQueueCheckpointData2NV))
            {
                Logger::warn("VK_NV_device_diagnostic_checkpoints enabled but checkpoint query entry points are unavailable");
                logicalDevice->supportsNvDiagnosticCheckpoints = false;
            }

            if (logicalDevice->supportsDeviceFaultExt && !logicalDevice->vkd.GetDeviceFaultInfoEXT)
            {
                Logger::warn("VK_EXT_device_fault enabled but vkGetDeviceFaultInfoEXT entry point is unavailable");
                logicalDevice->supportsDeviceFaultExt = false;
            }

            Logger::info(
                std::string("GPU crash diagnostics enabled (NV checkpoints=")
                + (logicalDevice->supportsNvDiagnosticCheckpoints ? "on" : "off")
                + ", NV diagnostics config=" + (logicalDevice->supportsNvDiagnosticsConfig ? "on" : "off")
                + ", device fault=" + (logicalDevice->supportsDeviceFaultExt ? "on" : "off") + ")");
        }

        uint32_t count;

        logicalDevice->vki.GetPhysicalDeviceQueueFamilyProperties(logicalDevice->physicalDevice, &count, nullptr);

        std::vector<VkQueueFamilyProperties> queueProperties(count);

        logicalDevice->vki.GetPhysicalDeviceQueueFamilyProperties(logicalDevice->physicalDevice, &count, queueProperties.data());
        for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++)
        {
            auto& queueInfo = pCreateInfo->pQueueCreateInfos[i];
            if (queueInfo.queueFamilyIndex < queueProperties.size()
                && (queueProperties[queueInfo.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            {
                logicalDevice->vkd.GetDeviceQueue(logicalDevice->device, queueInfo.queueFamilyIndex, 0, &logicalDevice->queue);

                VkCommandPoolCreateInfo commandPoolCreateInfo;
                commandPoolCreateInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                commandPoolCreateInfo.pNext            = nullptr;
                commandPoolCreateInfo.flags            = 0;
                commandPoolCreateInfo.queueFamilyIndex = queueInfo.queueFamilyIndex;

                Logger::debug("Found graphics capable queue");
                VkResult poolRes = logicalDevice->vkd.CreateCommandPool(logicalDevice->device, &commandPoolCreateInfo, nullptr, &logicalDevice->commandPool);
                if (poolRes != VK_SUCCESS)
                {
                    Logger::err("CreateCommandPool failed: " + std::to_string(poolRes));
                }
                logicalDevice->queueFamilyIndex = queueInfo.queueFamilyIndex;

                initializeDispatchTable(logicalDevice->queue, logicalDevice->device);

                break;
            }
        }

        if (!logicalDevice->queue)
        {
            Logger::err("Did not find a graphics queue! VKIntox requires a graphics-capable queue.");
            // Still register the device so destruction works, but effects won't function
        }

        deviceMap[GetKey(*pDevice)] = logicalDevice;

        return VK_SUCCESS;
    }

    void VKAPI_CALL VKIntox_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
    {
        if (!device)
            return;

        scoped_lock l(globalLock);

        Logger::trace("vkDestroyDevice");

        // Last chance to release anything still queued: once the device below is
        // destroyed, calling Vulkan against it is undefined behaviour, so the
        // queue is drained while the handle is still valid. Swapchain destruction
        // will normally have flushed already; this covers the paths that destroy
        // the device without a swapchain teardown, such as a lost device.
        DeferredDestroyQueue::instance().flush();

        auto devIt = deviceMap.find(GetKey(device));
        if (devIt == deviceMap.end() || !devIt->second)
        {
            // Not created through this layer; nothing to clean up
            return;
        }
        LogicalDevice* logicalDevice = devIt->second.get();

        // Destroy all swapchains belonging to this device first
        for (auto swapIt = swapchainMap.begin(); swapIt != swapchainMap.end(); )
        {
            if (swapIt->second && swapIt->second->logicalDevice == logicalDevice)
            {
                swapIt->second->destroy();
                swapIt = swapchainMap.erase(swapIt);
            }
            else
            {
                ++swapIt;
            }
        }

        // Destroy ImGui overlay before device (it uses device resources)
        logicalDevice->imguiOverlay.reset();

        // Destroy persistent depth storage
        destroyPersistentDepthStorage(logicalDevice);

        // Destroy depth copy ring buffer pool and its fences. The teardown takes
        // the ring's own lock so it cannot race a QueueSubmit mid-copy; lock
        // order here is globalLock -> depthCopy, as DepthCopyState documents.
        logicalDevice->depthCopy.withRing([&](DepthCopyState::RingAccess& ring) {
            VkCommandPool pool = VK_NULL_HANDLE;
            std::vector<VkFence> fences;
            ring.detach(pool, fences);

            if (pool == VK_NULL_HANDLE && fences.empty())
                return;

            Logger::debug("DestroyCommandPool (depth copy ring buffer)");
            // Destroy fences first (they're independent of the pool)
            for (VkFence f : fences)
            {
                if (f != VK_NULL_HANDLE)
                    logicalDevice->vkd.DestroyFence(device, f, nullptr);
            }
            if (pool != VK_NULL_HANDLE)
                logicalDevice->vkd.DestroyCommandPool(device, pool, pAllocator);
        });

        // Clean up Wayland input resources (no-op if not initialized)
        cleanupWaylandKeyboard();
        cleanupWaylandMouse();

        if (logicalDevice->commandPool != VK_NULL_HANDLE)
        {
            Logger::debug("DestroyCommandPool");
            logicalDevice->vkd.DestroyCommandPool(device, logicalDevice->commandPool, pAllocator);
        }

        // second drain: imguiOverlay.reset() queues the overlay's handles just before
        // the device dies, and there's no later flush to pick them up.
        DeferredDestroyQueue::instance().flush();

        if (logicalDevice->pipelineCache != VK_NULL_HANDLE)
        {
            logicalDevice->vkd.DestroyPipelineCache(device, logicalDevice->pipelineCache, pAllocator);
            logicalDevice->pipelineCache = VK_NULL_HANDLE;
        }

        logicalDevice->vkd.DestroyDevice(device, pAllocator);

        depthRetryStates.erase(logicalDevice);
        {
            std::lock_guard<std::mutex> lossLock(deviceLossLock);
            deviceLostDevices.erase(logicalDevice);
        }
        deviceMap.erase(GetKey(device));
    }

    VKAPI_ATTR VkResult VKAPI_CALL VKIntox_CreateSwapchainKHR(VkDevice                        device,
                                                               const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                               const VkAllocationCallbacks*    pAllocator,
                                                               VkSwapchainKHR*                 swapchain)
    {
        scoped_lock l(globalLock);

        Logger::trace("vkCreateSwapchainKHR");

        LogicalDevice* logicalDevice = deviceMap[GetKey(device)].get();
        if (logicalDevice == nullptr)
            return VK_ERROR_DEVICE_LOST;

        VkSwapchainCreateInfoKHR modifiedCreateInfo = *pCreateInfo;

        VkFormat format = modifiedCreateInfo.imageFormat;

        VkFormat srgbFormat  = isSRGB(format) ? format : convertToSRGB(format);
        VkFormat unormFormat = isSRGB(format) ? convertToUNORM(format) : format;
        Logger::debug(std::to_string(srgbFormat) + " " + std::to_string(unormFormat));

        VkFormat formats[] = {unormFormat, srgbFormat};

        VkImageFormatListCreateInfoKHR imageFormatListCreateInfo;
        bool hasMutableEnvOverride = false;
        bool useMutableFormat = getMutableSwapchainEnvOverride(hasMutableEnvOverride, logicalDevice->supportsMutableFormat);
        if (useMutableFormat && !logicalDevice->supportsMutableFormat)
        {
            Logger::warn("Mutable swapchain forced on via VKINTOX_ENABLE_MUTABLE_SWAPCHAIN, but device does not support it. Falling back to disabled.");
            useMutableFormat = false;
        }
        else if (useMutableFormat &&
                 pNextChainContainsSType(modifiedCreateInfo.pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO))
        {
            Logger::warn("Application already provides VkImageFormatListCreateInfo in swapchain pNext; disabling mutable override for compatibility.");
            useMutableFormat = false;
        }
        else if (hasMutableEnvOverride)
        {
            Logger::info(std::string("Mutable swapchain explicitly set via VKINTOX_ENABLE_MUTABLE_SWAPCHAIN=") + (useMutableFormat ? "1" : "0"));
        }

        if (useMutableFormat)
        {
            // Keep application-requested usage bits and add the ones VKIntox needs.
            modifiedCreateInfo.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                             ; // mutable path renders final pass directly into swapchain
            modifiedCreateInfo.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
            // TODO what if the application already uses multiple formats for the swapchain?

            imageFormatListCreateInfo.sType           = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR;
            imageFormatListCreateInfo.pNext           = modifiedCreateInfo.pNext;
            imageFormatListCreateInfo.viewFormatCount = (srgbFormat == unormFormat) ? 1 : 2;
            imageFormatListCreateInfo.pViewFormats    = formats;

            modifiedCreateInfo.pNext = &imageFormatListCreateInfo;
        }

        // Keep application-provided usage bits unchanged on the non-mutable path.
        // Some drivers/apps are sensitive to swapchain usage mutation.

        if (isSwapchainDiagEnabled())
        {
            std::fprintf(stderr,
                         "VKIntox diag: CreateSwapchainKHR mutable=%d reqUsage=0x%x finalUsage=0x%x reqFlags=0x%x finalFlags=0x%x format=%u extent=%ux%u minImages=%u\n",
                         useMutableFormat ? 1 : 0,
                         static_cast<unsigned int>(pCreateInfo->imageUsage),
                         static_cast<unsigned int>(modifiedCreateInfo.imageUsage),
                         static_cast<unsigned int>(pCreateInfo->flags),
                         static_cast<unsigned int>(modifiedCreateInfo.flags),
                         static_cast<unsigned int>(modifiedCreateInfo.imageFormat),
                         static_cast<unsigned int>(modifiedCreateInfo.imageExtent.width),
                         static_cast<unsigned int>(modifiedCreateInfo.imageExtent.height),
                         static_cast<unsigned int>(modifiedCreateInfo.minImageCount));
        }

        Logger::debug("format " + std::to_string(modifiedCreateInfo.imageFormat));
        
        // Recreation (same handle) is detected AFTER vkCreateSwapchainKHR returns,
        // since *swapchain is uninitialized until then.
        auto logicalSwapchain = std::make_shared<LogicalSwapchain>();
        logicalSwapchain->logicalDevice      = logicalDevice;
        logicalSwapchain->swapchainCreateInfo = *pCreateInfo;
        logicalSwapchain->imageExtent         = modifiedCreateInfo.imageExtent;
        logicalSwapchain->format              = modifiedCreateInfo.imageFormat;
        logicalSwapchain->imageCount          = 0;
        logicalSwapchain->useMutableFormat    = useMutableFormat;

        VkResult result = logicalDevice->vkd.CreateSwapchainKHR(device, &modifiedCreateInfo, pAllocator, swapchain);

        if (result == VK_SUCCESS && swapchain != nullptr && *swapchain != VK_NULL_HANDLE)
        {
            // NOW check if this handle already existed (recreation scenario)
            uint64_t handle = (uint64_t)(uintptr_t)*swapchain;
            auto oldIt = swapchainMap.find((VkSwapchainKHR)handle); // Cast back or use proper key type
            
            // Since swapchain handles are opaque, we use the map key directly
            // If an entry with this key exists, we're recreating
            if (oldIt != swapchainMap.end() && oldIt->second)
            {
                Logger::warn("CreateSwapchainKHR: Recreating swapchain, destroying stale resources");

                // destroy the retired entry before dropping our last reference. overwriting
                // it leaked and raced every in-flight resource on each resize,
                // which is how rapid rebuild churn turned into device-lost.
                if (oldIt->second.get() != logicalSwapchain.get())
                    oldIt->second->destroy();

                // Replace the old entry
                swapchainMap[(VkSwapchainKHR)handle] = logicalSwapchain;
            }
            else
            {
                swapchainMap[(VkSwapchainKHR)handle] = logicalSwapchain;
            }
            
            // clear depth state so nothing stale survives into the new swapchain
            logicalSwapchain->depthResolveSourceView = VK_NULL_HANDLE;
            logicalSwapchain->depthReallocPending = true;
            
            logicalSwapchain->depthResolvePerImage.resize(modifiedCreateInfo.minImageCount);
            for (auto& perImg : logicalSwapchain->depthResolvePerImage) perImg.image = VK_NULL_HANDLE;
            
            // Also clear device-level depth state when swapchain is recreated (Roblox OTA scenario)
            logicalDevice->activeDepthState = {};
            logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
            logicalDevice->depthReallocPending = false;
            depthRetryStates.erase(logicalDevice);
            Logger::debug("CreateSwapchainKHR: cleared device depth state for fresh detection");
        }
        else
        {
            Logger::err("vkCreateSwapchainKHR failed: " + std::to_string(result));
        }

        return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL VKIntox_GetSwapchainImagesKHR(VkDevice       device,
                                                                  VkSwapchainKHR swapchain,
                                                                  uint32_t*      pCount,
                                                                  VkImage*       swapchainImages)
    {
        scoped_lock l(globalLock);
        if (pCount == nullptr)
            return VK_ERROR_INITIALIZATION_FAILED;
        Logger::trace("vkGetSwapchainImagesKHR " + std::to_string(*pCount));

        LogicalDevice* logicalDevice = deviceMap[GetKey(device)].get();
        if (logicalDevice == nullptr)
            return VK_ERROR_DEVICE_LOST;

        if (swapchainImages == nullptr)
        {
            return logicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, pCount, swapchainImages);
        }

        auto swapchainIt = swapchainMap.find(swapchain);
        if (swapchainIt == swapchainMap.end() || !swapchainIt->second)
            return logicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, pCount, swapchainImages);
        LogicalSwapchain* logicalSwapchain = swapchainIt->second.get();

        // If the images got already requested once, return them again instead of creating new images
        if (logicalSwapchain->fakeImages.size())
        {
            if (logicalSwapchain->fakeImages.size() < logicalSwapchain->imageCount)
            {
                Logger::err("fake image cache is smaller than imageCount");
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            *pCount = std::min<uint32_t>(*pCount, logicalSwapchain->imageCount);
            std::memcpy(swapchainImages, logicalSwapchain->fakeImages.data(), sizeof(VkImage) * (*pCount));
            return *pCount < logicalSwapchain->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
        }

        const uint32_t requestedImageCapacity = *pCount;
        uint32_t realImageCount = 0;
        VkResult getCountResult = logicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, &realImageCount, nullptr);
        if (isSwapchainDiagEnabled())
        {
            std::fprintf(stderr,
                         "VKIntox diag: GetSwapchainImagesKHR count-query result=%d count=%u\n",
                         static_cast<int>(getCountResult),
                         static_cast<unsigned int>(realImageCount));
        }
        if (getCountResult != VK_SUCCESS && getCountResult != VK_INCOMPLETE)
        {
            Logger::err("vkGetSwapchainImagesKHR(count) failed: " + std::to_string(getCountResult));
            return getCountResult;
        }
        if (realImageCount == 0)
        {
            Logger::err("vkGetSwapchainImagesKHR returned zero images");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkImage> realImages(realImageCount);
        uint32_t fetchedImageCount = realImageCount;
        VkResult getImagesResult = logicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, &fetchedImageCount, realImages.data());
        if (isSwapchainDiagEnabled())
        {
            std::fprintf(stderr,
                         "VKIntox diag: GetSwapchainImagesKHR fetch result=%d fetched=%u\n",
                         static_cast<int>(getImagesResult),
                         static_cast<unsigned int>(fetchedImageCount));
        }
        if (getImagesResult != VK_SUCCESS && getImagesResult != VK_INCOMPLETE)
        {
            Logger::err("vkGetSwapchainImagesKHR(images) failed: " + std::to_string(getImagesResult));
            return getImagesResult;
        }
        if (fetchedImageCount == 0)
        {
            Logger::err("vkGetSwapchainImagesKHR fetched zero images");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        realImages.resize(fetchedImageCount);
        logicalSwapchain->imageCount = fetchedImageCount;
        logicalSwapchain->images = std::move(realImages);

        // a surface neither backend processes gets the real images so it still
        // presents. wayland and x11 both get the fakes below; the present path
        // copies them into the real swapchain.
        if (!isWayland() && !isX11())
        {
            *pCount = std::min<uint32_t>(*pCount, logicalSwapchain->imageCount);
            std::memcpy(swapchainImages, logicalSwapchain->images.data(), sizeof(VkImage) * (*pCount));
            return *pCount < logicalSwapchain->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
        }

        // Create image views for overlay rendering
        logicalSwapchain->imageViews.resize(logicalSwapchain->imageCount);
        for (uint32_t i = 0; i < logicalSwapchain->imageCount; i++)
        {
            VkImageViewCreateInfo viewInfo = {};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = logicalSwapchain->images[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = logicalSwapchain->format;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;
            VkResult viewResult = logicalDevice->vkd.CreateImageView(logicalDevice->device, &viewInfo, nullptr, &logicalSwapchain->imageViews[i]);
            if (viewResult != VK_SUCCESS)
                Logger::err("Failed to create swapchain image view " + std::to_string(i) + ": " + std::to_string(viewResult));
        }

        // Initialize registry from config on first run (before calculating effect slots)
        bool isFirstRun = !effectRegistry.isInitializedFromConfig();
        if (isFirstRun)
            effectRegistry.initializeSelectedEffectsFromConfig();

        const auto& selectedEffects = effectRegistry.getSelectedEffects();

        // Allow dynamic effect loading by allocating for more effects than configured.
        // Clamp maxEffects to a safe range to avoid pathological allocations.
        int32_t maxEffects = std::clamp(settingsManager.getMaxEffects(), 1, 200);
        size_t effectSlots = std::max(selectedEffects.size(), static_cast<size_t>(maxEffects));
        logicalSwapchain->maxEffectSlots = effectSlots;

        // create 1 more set of images when we can't use the swapchain itself
        const uint64_t slotCount = static_cast<uint64_t>(effectSlots) + (logicalSwapchain->useMutableFormat ? 0u : 1u);
        const uint64_t fakeImageCount64 = static_cast<uint64_t>(logicalSwapchain->imageCount) * slotCount;
        if (fakeImageCount64 == 0 || fakeImageCount64 > std::numeric_limits<uint32_t>::max())
        {
            Logger::err("Invalid fake image count computed: " + std::to_string(fakeImageCount64));
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        uint32_t fakeImageCount = static_cast<uint32_t>(fakeImageCount64);

        logicalSwapchain->fakeImages =
            createFakeSwapchainImages(logicalDevice, logicalSwapchain->swapchainCreateInfo, fakeImageCount, logicalSwapchain->fakeImageMemories);
        if (logicalSwapchain->fakeImages.empty())
        {
            Logger::err("Failed to create fake swapchain images");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        Logger::debug("created fake swapchain images");

        if (!isFirstRun && !selectedEffects.empty())
        {
            // Resize with effects - use pass-through and debounce for smooth resize
            Logger::debug("using pass-through during resize, will restore effects after debounce");
            std::vector<VkImage> firstImages(logicalSwapchain->fakeImages.begin(),
                                             logicalSwapchain->fakeImages.begin() + logicalSwapchain->imageCount);
            logicalSwapchain->effects.push_back(std::make_shared<TransferEffect>(
                logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent,
                firstImages, logicalSwapchain->images, config.get()));

            resizeDebounce.pending = true;
            resizeDebounce.lastResizeTime = std::chrono::steady_clock::now();
        }
        else
        {
            // First run OR empty effects - create effects from registry
            createEffectsForSwapchain(logicalSwapchain, logicalDevice, config.get(), selectedEffects, true);
        }

        DepthState depth = getDepthState(logicalDevice);

        Logger::debug("selected effect count: " + std::to_string(selectedEffects.size()));
        Logger::debug("effect count: " + std::to_string(logicalSwapchain->effects.size()));

        logicalSwapchain->commandBuffersEffect = allocateCommandBuffer(logicalDevice, logicalSwapchain->imageCount);
        Logger::debug("allocated ComandBuffers " + std::to_string(logicalSwapchain->commandBuffersEffect.size()) + " for swapchain "
                      + convertToString(swapchain));

        ensureDepthResolveResources(logicalSwapchain, depth);
        writeCommandBuffers(logicalDevice,
                            logicalSwapchain,
                            logicalSwapchain->effects,
                            logicalSwapchain->commandBuffersEffect,
                            depth);
        Logger::debug("wrote CommandBuffers");

        logicalSwapchain->semaphores = createSemaphores(logicalDevice, logicalSwapchain->imageCount);
        logicalSwapchain->overlaySemaphores = createSemaphores(logicalDevice, logicalSwapchain->imageCount);

        // Create per-image fences for effect CB submission tracking.
        // These ensure we don't update descriptor sets or free CBs while
        // the GPU is still using them (which causes VK_ERROR_DEVICE_LOST).
        logicalSwapchain->effectSubmitFences.resize(logicalSwapchain->imageCount, VK_NULL_HANDLE);
        logicalSwapchain->effectSubmitFenceUsed.resize(logicalSwapchain->imageCount, false);
        VkFenceCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        for (uint32_t i = 0; i < logicalSwapchain->imageCount; ++i)
        {
            VkResult fr = logicalDevice->vkd.CreateFence(logicalDevice->device, &fci, nullptr, &logicalSwapchain->effectSubmitFences[i]);
            if (fr != VK_SUCCESS)
            {
                Logger::err("Failed to create effect submit fence for image " + std::to_string(i) + ": " + std::to_string(fr));
                logicalSwapchain->effectSubmitFences[i] = VK_NULL_HANDLE;
            }
        }

        Logger::debug("created semaphores + fences");
        for (unsigned int i = 0; i < logicalSwapchain->imageCount; i++)
        {
            Logger::debug(std::to_string(i) + " written commandbuffer " + convertToString(logicalSwapchain->commandBuffersEffect[i]));
        }
        Logger::trace("vkGetSwapchainImagesKHR");

        logicalSwapchain->defaultTransfer = std::make_shared<TransferEffect>(
            logicalDevice,
            logicalSwapchain->format,
            logicalSwapchain->imageExtent,
            std::vector<VkImage>(logicalSwapchain->fakeImages.begin(), logicalSwapchain->fakeImages.begin() + logicalSwapchain->imageCount),
            logicalSwapchain->images,
            config.get());

        logicalSwapchain->commandBuffersNoEffect = allocateCommandBuffer(logicalDevice, logicalSwapchain->imageCount);

        writeCommandBuffers(logicalDevice,
                            logicalSwapchain,
                            {logicalSwapchain->defaultTransfer},
                            logicalSwapchain->commandBuffersNoEffect,
                            depth);

        for (unsigned int i = 0; i < logicalSwapchain->imageCount; i++)
        {
            Logger::debug(std::to_string(i) + " written commandbuffer " + convertToString(logicalSwapchain->commandBuffersNoEffect[i]));
        }

        // Create ImGui overlay at device level (if not already created)
        // This survives swapchain recreation during resize
        if (!logicalDevice->imguiOverlay)
        {
            if (!logicalDevice->overlayPersistentState)
                logicalDevice->overlayPersistentState = std::make_unique<OverlayPersistentState>();
            logicalDevice->imguiOverlay = std::make_unique<ImGuiOverlay>(
                logicalDevice, logicalSwapchain->format, logicalSwapchain->imageCount,
                logicalDevice->overlayPersistentState.get());
            // Set the effect registry pointer (single source of truth for enabled states)
            logicalDevice->imguiOverlay->setEffectRegistry(&effectRegistry);

            // Set game/profile info for auto-save
            logicalDevice->imguiOverlay->setGameProfile(detectedGameName, activeProfileName, activeProfilePath);

            // Initialize input blocking (grabs all input when overlay is visible)
            static bool inputBlockerInited = false;
            if (!inputBlockerInited)
            {
                initInputBlocker(settingsManager.getOverlayBlockInput());
                if (logicalDevice->imguiOverlay)
                    setInputBlocked(logicalDevice->imguiOverlay->isVisible());
                inputBlockerInited = true;
            }
        }

        if (logicalSwapchain->fakeImages.size() < logicalSwapchain->imageCount)
        {
            Logger::err("fake image vector too small for swapchain copy");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        *pCount = std::min<uint32_t>(requestedImageCapacity, logicalSwapchain->imageCount);
        std::memcpy(swapchainImages, logicalSwapchain->fakeImages.data(), sizeof(VkImage) * (*pCount));
        return requestedImageCapacity < logicalSwapchain->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
    }

    // --- v3 Deferred Depth Copy: QueueSubmit interception ---
    // When depthCaptureMethod == 1 or 2, we intercept QueueSubmit to inject a
    // depth copy command buffer into the last submit. The copy is recorded
    // into a ring-buffered CB from a dedicated transient pool, using the
    // correct source layout obtained from the render pass's depth attachment
    // finalLayout at CmdBeginRenderPass time.
    //
    // The pending copy and the ring live in DepthCopyState and are guarded by
    // its own mutex, not globalLock: panicLayer() below flushes the deferred
    // destroy queue, which must never happen with globalLock held. The lock
    // order is globalLock -> depthCopy, so nothing in withRing() touches
    // globalLock-backed state; storage is prepared before and published after.
    VKAPI_ATTR VkResult VKAPI_CALL VKIntox_QueueSubmit(VkQueue queue,
                                                       uint32_t submitCount,
                                                       const VkSubmitInfo* pSubmits,
                                                       VkFence fence)
    {
        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(queue));
            if (devIt != deviceMap.end())
                logicalDevice = devIt->second.get();
        }

        const int method = settingsManager.getDepthCaptureMethod();
        if (!logicalDevice || method < 1 || method > 2
            || !settingsManager.getDepthCapture()
            || logicalDevice->softDisabled.load(std::memory_order_acquire)
            || submitCount == 0 || !pSubmits)
        {
            if (logicalDevice)
            {
                VkResult passthroughResult = logicalDevice->vkd.QueueSubmit(queue, submitCount, pSubmits, fence);
                reportDeviceLostDiagnostics(logicalDevice, queue, "vkQueueSubmit(passthrough)", passthroughResult);
                if (passthroughResult == VK_ERROR_DEVICE_LOST)
                    panicLayer(logicalDevice, "Vulkan device lost during QueueSubmit passthrough");
                return passthroughResult;
            }
            return reinterpret_cast<PFN_vkQueueSubmit>(dlsym(RTLD_NEXT, "vkQueueSubmit"))(queue, submitCount, pSubmits, fence);
        }

        // Claim the pending copy. Clearing the flag is unconditional and happens
        // under the depth-copy lock, so a copy can be published mid-read without
        // tearing the DepthState and without two submits racing for it.
        DepthState captureDepth;
        VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!logicalDevice->depthCopy.consume(captureDepth, sourceLayout))
        {
            VkResult passthroughResult = logicalDevice->vkd.QueueSubmit(queue, submitCount, pSubmits, fence);
            reportDeviceLostDiagnostics(logicalDevice, queue, "vkQueueSubmit(passthrough)", passthroughResult);
            if (passthroughResult == VK_ERROR_DEVICE_LOST)
                panicLayer(logicalDevice, "Vulkan device lost during QueueSubmit passthrough");
            return passthroughResult;
        }

        // Validate the tracked maps under globalLock, but drop it before any
        // panic: panicLayer() -> flush() is not allowed under globalLock.
        bool depthValid = false;
        {
            scoped_lock l(globalLock);
            depthValid = hasDepthState(captureDepth) && validateDepthStateForResolve(logicalDevice, captureDepth);
        }
        if (!depthValid)
        {
            Logger::debug("QueueSubmit v3: skipping invalid/stale depth state");
            VkResult passthroughResult = logicalDevice->vkd.QueueSubmit(queue, submitCount, pSubmits, fence);
            reportDeviceLostDiagnostics(logicalDevice, queue, "vkQueueSubmit(passthrough-invalid-depth)", passthroughResult);
            if (passthroughResult == VK_ERROR_DEVICE_LOST)
                panicLayer(logicalDevice, "Vulkan device lost during invalid-depth passthrough");
            return passthroughResult;
        }

        // Prepare the storage under globalLock and snapshot what the recording
        // needs into locals, so the ring section below holds only depthCopy.
        const VkFormat storageFormat = (sourceLayout == VK_IMAGE_LAYOUT_GENERAL)
            ? VK_FORMAT_R32_SFLOAT
            : captureDepth.format;

        VkImage storageImage = VK_NULL_HANDLE;
        VkImageView storageImageView = VK_NULL_HANDLE;
        bool storageWasValid = false;
        {
            scoped_lock l(globalLock);
            ensurePersistentDepthStorage(logicalDevice, storageFormat, captureDepth.extent);
            const auto& storage = logicalDevice->depthCaptureStorage;
            storageImage = storage.image;
            storageImageView = storage.view;
            storageWasValid = storage.valid;
        }

        if (storageImage == VK_NULL_HANDLE)
        {
            VkResult passthroughResult = logicalDevice->vkd.QueueSubmit(queue, submitCount, pSubmits, fence);
            reportDeviceLostDiagnostics(logicalDevice, queue, "vkQueueSubmit(passthrough-no-depth-storage)", passthroughResult);
            if (passthroughResult == VK_ERROR_DEVICE_LOST)
                panicLayer(logicalDevice, "Vulkan device lost during no-depth-storage passthrough");
            return passthroughResult;
        }

        // --- ring section: init, reserve, fence, record, submit ---
        bool ringBusy = false;            // slot in flight or ring unavailable: skip the copy
        VkResult slotFenceError = VK_SUCCESS;
        VkResult submitResult = VK_SUCCESS;

        logicalDevice->depthCopy.withRing([&](DepthCopyState::RingAccess& ring) {
            if (!ring.ready())
            {
                VkCommandPoolCreateInfo poolCI = {};
                poolCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                poolCI.queueFamilyIndex = logicalDevice->queueFamilyIndex;

                VkCommandPool pool = VK_NULL_HANDLE;
                VkResult poolResult = logicalDevice->vkd.CreateCommandPool(logicalDevice->device, &poolCI, nullptr, &pool);
                if (poolResult != VK_SUCCESS || pool == VK_NULL_HANDLE)
                {
                    Logger::err("QueueSubmit v3: could not create depth copy pool (" + std::to_string(poolResult) + ")");
                    ringBusy = true;
                    return;
                }

                std::vector<VkCommandBuffer> buffers(DepthCopyState::RING_SIZE, VK_NULL_HANDLE);
                VkCommandBufferAllocateInfo cbai = {};
                cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                cbai.commandPool = pool;
                cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                cbai.commandBufferCount = DepthCopyState::RING_SIZE;
                VkResult cbResult = logicalDevice->vkd.AllocateCommandBuffers(logicalDevice->device, &cbai, buffers.data());
                if (cbResult != VK_SUCCESS)
                {
                    Logger::err("QueueSubmit v3: could not allocate depth copy command buffers (" + std::to_string(cbResult) + ")");
                    logicalDevice->vkd.DestroyCommandPool(logicalDevice->device, pool, nullptr);
                    ringBusy = true;
                    return;
                }

                for (auto cb : buffers)
                    initializeDispatchTable(cb, logicalDevice->device);

                std::vector<VkFence> fences(DepthCopyState::RING_SIZE, VK_NULL_HANDLE);
                VkFenceCreateInfo fci = {};
                fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                bool fencesOk = true;
                for (uint32_t i = 0; i < DepthCopyState::RING_SIZE; ++i)
                {
                    VkResult fr = logicalDevice->vkd.CreateFence(logicalDevice->device, &fci, nullptr, &fences[i]);
                    if (fr != VK_SUCCESS)
                    {
                        Logger::err("QueueSubmit v3: failed to create depth copy fence for slot " + std::to_string(i) + " (" + std::to_string(fr) + ")");
                        fencesOk = false;
                        break;
                    }
                }
                if (!fencesOk)
                {
                    for (VkFence f : fences)
                        if (f != VK_NULL_HANDLE)
                            logicalDevice->vkd.DestroyFence(logicalDevice->device, f, nullptr);
                    logicalDevice->vkd.DestroyCommandPool(logicalDevice->device, pool, nullptr);
                    ringBusy = true;
                    return;
                }

                ring.install(pool, std::move(buffers), std::move(fences));
                Logger::debug("depth copy ring buffer initialized: " + std::to_string(DepthCopyState::RING_SIZE) + " CBs + fences");
            }

            const uint32_t slotIndex = ring.reserveSlot();
            VkCommandBuffer copyCB = ring.buffer(slotIndex);
            VkFence slotFence = ring.fence(slotIndex);

            // never block on a ring slot. an unsignaled fence means the old buffer is still
            // in flight: skip the optional copy and let the app's submit proceed, or a
            // stalled GPU becomes a multi-second present-thread freeze.
            if (slotFence != VK_NULL_HANDLE)
            {
                auto getFenceStatus = reinterpret_cast<PFN_vkGetFenceStatus>(
                    logicalDevice->vkd.GetDeviceProcAddr(logicalDevice->device, "vkGetFenceStatus"));
                VkResult fenceStatus = getFenceStatus(logicalDevice->device, slotFence);
                if (fenceStatus == VK_NOT_READY)
                {
                    Logger::debug("QueueSubmit v3: depth copy ring slot "
                                  + std::to_string(slotIndex) + " still in flight; skipping optional depth copy");
                    ringBusy = true;
                    return;
                }
                if (fenceStatus != VK_SUCCESS)
                {
                    slotFenceError = fenceStatus;
                    return;
                }
                logicalDevice->vkd.ResetFences(logicalDevice->device, 1, &slotFence);
            }

            logicalDevice->vkd.ResetCommandBuffer(copyCB, 0);

            const bool isMsaa = captureDepth.samples != VK_SAMPLE_COUNT_1_BIT;
            const VkImageAspectFlags depthAspect = isStencilFormat(captureDepth.format)
                ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                : VK_IMAGE_ASPECT_DEPTH_BIT;

            // Record the copy
            VkCommandBufferBeginInfo cbbi = {};
            cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            logicalDevice->vkd.BeginCommandBuffer(copyCB, &cbbi);

            // Barrier: source from KNOWN sourceLayout → TRANSFER_SRC
            VkImageMemoryBarrier barriers[2] = {};
            barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[0].image = captureDepth.image;
            barriers[0].oldLayout = (sourceLayout != VK_IMAGE_LAYOUT_UNDEFINED) ? sourceLayout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barriers[0].srcAccessMask = (sourceLayout == VK_IMAGE_LAYOUT_GENERAL)
                ? (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)
                : (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[0].subresourceRange = {depthAspect, 0, 1, 0, 1};

            // Barrier: storage → TRANSFER_DST
            barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[1].image = storageImage;
            barriers[1].oldLayout = storageWasValid ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barriers[1].srcAccessMask = storageWasValid ? VK_ACCESS_SHADER_READ_BIT : 0;
            barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

            VkPipelineStageFlags srcStages = (sourceLayout == VK_IMAGE_LAYOUT_GENERAL)
                ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                : (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);

            logicalDevice->vkd.CmdPipelineBarrier(copyCB, srcStages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

            if (isMsaa)
            {
                VkImageResolve resolveRegion = {};
                resolveRegion.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
                resolveRegion.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
                resolveRegion.extent = {captureDepth.extent.width, captureDepth.extent.height, 1};
                logicalDevice->vkd.CmdResolveImage(copyCB, captureDepth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, storageImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &resolveRegion);
            }
            else
            {
                VkImageCopy copyRegion = {};
                copyRegion.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
                copyRegion.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
                copyRegion.extent = {captureDepth.extent.width, captureDepth.extent.height, 1};
                logicalDevice->vkd.CmdCopyImage(copyCB, captureDepth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, storageImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
            }

            // Barrier: storage → SHADER_READ_ONLY
            VkImageMemoryBarrier readOnlyBarrier = {};
            readOnlyBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            readOnlyBarrier.image = storageImage;
            readOnlyBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            readOnlyBarrier.newLayout = isStencilFormat(storageFormat) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
            readOnlyBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            readOnlyBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            readOnlyBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            readOnlyBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            readOnlyBarrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            logicalDevice->vkd.CmdPipelineBarrier(copyCB, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &readOnlyBarrier);

            logicalDevice->vkd.EndCommandBuffer(copyCB);

            Logger::debug("QueueSubmit v3: depth copy via ring CB index=" + std::to_string(slotIndex)
                          + " " + std::to_string(captureDepth.extent.width) + "x" + std::to_string(captureDepth.extent.height)
                          + " fmt=" + std::to_string(storageFormat)
                          + " srcLayout=" + std::to_string(static_cast<uint32_t>(sourceLayout))
                          + (isMsaa ? " [MSAA resolve]" : " [copy]"));

            // Inject copyCB into the last submit's command buffer list
            const VkSubmitInfo& lastSubmit = pSubmits[submitCount - 1];
            std::vector<VkCommandBuffer> combinedCmdBufs(lastSubmit.pCommandBuffers, lastSubmit.pCommandBuffers + lastSubmit.commandBufferCount);
            combinedCmdBufs.push_back(copyCB);

            VkSubmitInfo modifiedLastSubmit = lastSubmit;
            modifiedLastSubmit.commandBufferCount = static_cast<uint32_t>(combinedCmdBufs.size());
            modifiedLastSubmit.pCommandBuffers = combinedCmdBufs.data();

            // Submit — no splitting, just replace the last submit. The slot fence
            // is signaled on completion so the slot can be safely reused.
            if (submitCount <= 1)
            {
                submitResult = logicalDevice->vkd.QueueSubmit(queue, 1, &modifiedLastSubmit, slotFence);
            }
            else
            {
                submitResult = logicalDevice->vkd.QueueSubmit(queue, submitCount - 1, pSubmits, VK_NULL_HANDLE);
                if (submitResult == VK_SUCCESS)
                    submitResult = logicalDevice->vkd.QueueSubmit(queue, 1, &modifiedLastSubmit, slotFence);
            }
        });

        if (ringBusy)
        {
            VkResult passthroughResult = logicalDevice->vkd.QueueSubmit(queue, submitCount, pSubmits, fence);
            reportDeviceLostDiagnostics(logicalDevice, queue, "vkQueueSubmit(passthrough-ring-busy)", passthroughResult);
            if (passthroughResult == VK_ERROR_DEVICE_LOST)
                panicLayer(logicalDevice, "Vulkan device lost during QueueSubmit passthrough");
            return passthroughResult;
        }

        if (slotFenceError != VK_SUCCESS)
        {
            if (slotFenceError == VK_ERROR_DEVICE_LOST)
            {
                reportDeviceLostDiagnostics(logicalDevice, queue, "QueueSubmit v3 depth-copy fence status", slotFenceError);
                panicLayer(logicalDevice, "Device lost during depth-copy ring fence status");
                return slotFenceError;
            }
            Logger::warn("QueueSubmit v3: depth copy ring fence status failed (" + std::to_string(slotFenceError) + ")");
            VkResult passthroughResult = logicalDevice->vkd.QueueSubmit(queue, submitCount, pSubmits, fence);
            reportDeviceLostDiagnostics(logicalDevice, queue, "vkQueueSubmit(passthrough-ring-status)", passthroughResult);
            if (passthroughResult == VK_ERROR_DEVICE_LOST)
                panicLayer(logicalDevice, "Vulkan device lost during QueueSubmit passthrough");
            return passthroughResult;
        }

        // The copy went out; publish the storage as the deepest state and let the
        // barrier in the next copy treat its contents as valid. If the submit
        // failed the storage is left marked invalid so the next copy starts from
        // UNDEFINED rather than assuming contents that never landed.
        if (submitResult == VK_SUCCESS)
        {
            scoped_lock l(globalLock);
            logicalDevice->depthCaptureStorage.valid = true;
            DepthState storageState = captureDepth;
            storageState.image = storageImage;
            storageState.imageView = storageImageView;
            storageState.format = storageFormat;
            storageState.samples = VK_SAMPLE_COUNT_1_BIT;
            storageState.transient = false;
            storageState.observedLayout = isStencilFormat(storageFormat)
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
            updateDeviceDepthStateLocked(logicalDevice, storageState, "QueueSubmit v3");
        }

        reportDeviceLostDiagnostics(logicalDevice, queue, "vkQueueSubmit(depth-copy)", submitResult);
        if (submitResult == VK_ERROR_DEVICE_LOST)
            panicLayer(logicalDevice, "Vulkan device lost during depth-copy submit");
        return submitResult;
    }

    VKAPI_ATTR VkResult VKAPI_CALL VKIntox_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
    {
        // Soft-disable fast path: panicLayer() has flipped this bit. The layer
        // is no longer trusted to touch the GPU, so we pass straight through
        // to the driver — except we still submit an overlay-only frame so any
        // pending toast notification stays visible. The overlay's render pass
        // only writes to the swapchain image, so the present must wait on the
        // overlay's signal semaphore.
        {
            std::shared_ptr<LogicalDevice> passThroughDevice;
            bool softDisabled = false;
            bool hasToasts = false;
            {
                scoped_lock l(globalLock);
                auto devIt = deviceMap.find(GetKey(queue));
                if (devIt == deviceMap.end() || !devIt->second)
                    return VK_ERROR_DEVICE_LOST;
                passThroughDevice = devIt->second;
                softDisabled = passThroughDevice->softDisabled.load(std::memory_order_acquire);
                bool deviceLost = false;
                {
                    std::lock_guard<std::mutex> lossLock(deviceLossLock);
                    deviceLost = deviceLostDevices.find(passThroughDevice.get()) != deviceLostDevices.end();
                }
                if (softDisabled && deviceLost)
                    hasToasts = false;
                if (softDisabled && passThroughDevice->imguiOverlay && !deviceLost)
                    hasToasts = passThroughDevice->imguiOverlay->hasPendingToasts();
            }

            if (softDisabled)
            {
                if (!hasToasts || !passThroughDevice->imguiOverlay || pPresentInfo->swapchainCount == 0)
                {
                    VkResult vr = passThroughDevice->vkd.QueuePresentKHR(queue, pPresentInfo);
                    reportDeviceLostDiagnostics(passThroughDevice.get(), queue, "vkQueuePresentKHR(soft-disabled)", vr);
                    if (vr == VK_ERROR_DEVICE_LOST)
                        panicLayer(passThroughDevice.get(), "Vulkan device lost during soft-disabled present");
                    return vr;
                }

                // Submit an overlay-only frame for each swapchain, chained onto
                // the original wait semaphores so the present stays ordered.
                static thread_local std::vector<VkSemaphore> presentWaitSems;
                presentWaitSems.clear();
                presentWaitSems.reserve(pPresentInfo->swapchainCount);

                VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                for (unsigned int i = 0; i < pPresentInfo->swapchainCount; i++)
                {
                    auto swapIt = swapchainMap.find(pPresentInfo->pSwapchains[i]);
                    if (swapIt == swapchainMap.end() || !swapIt->second)
                    {
                        presentWaitSems.push_back(
                            i < pPresentInfo->waitSemaphoreCount ? pPresentInfo->pWaitSemaphores[i] : VK_NULL_HANDLE);
                        continue;
                    }
                    LogicalSwapchain* swap = swapIt->second.get();
                    uint32_t index = pPresentInfo->pImageIndices[i];
                    if (index >= swap->imageCount || index >= swap->overlaySemaphores.size())
                    {
                        presentWaitSems.push_back(VK_NULL_HANDLE);
                        continue;
                    }

                    VkCommandBuffer overlayCmd = passThroughDevice->imguiOverlay->recordFrame(
                        index, swap->imageViews[index],
                        swap->imageExtent.width, swap->imageExtent.height);

                    if (overlayCmd == VK_NULL_HANDLE)
                    {
                        // No overlay work this frame — fall back to waiting on the
                        // original semaphore (or none if there isn't one).
                        presentWaitSems.push_back(
                            i < pPresentInfo->waitSemaphoreCount ? pPresentInfo->pWaitSemaphores[i] : VK_NULL_HANDLE);
                        continue;
                    }

                    VkSubmitInfo oi = {};
                    oi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                    oi.pWaitDstStageMask = &waitStage;
                    if (i < pPresentInfo->waitSemaphoreCount && pPresentInfo->pWaitSemaphores[i] != VK_NULL_HANDLE)
                    {
                        oi.waitSemaphoreCount = 1;
                        oi.pWaitSemaphores = &pPresentInfo->pWaitSemaphores[i];
                    }
                    oi.commandBufferCount = 1;
                    oi.pCommandBuffers = &overlayCmd;
                    oi.signalSemaphoreCount = 1;
                    oi.pSignalSemaphores = &swap->overlaySemaphores[index];

                    VkFence overlayFence = passThroughDevice->imguiOverlay->getCommandBufferFence(index);
                    VkResult vr = passThroughDevice->vkd.QueueSubmit(
                        passThroughDevice->queue, 1, &oi, overlayFence);
                    if (vr != VK_SUCCESS)
                    {
                        Logger::err("Toast-only overlay submit failed: " + std::to_string(vr));
                        if (vr == VK_ERROR_DEVICE_LOST)
                            panicLayer(passThroughDevice.get(), "Vulkan device lost during toast-only overlay submit");
                        VkResult passthroughResult = passThroughDevice->vkd.QueuePresentKHR(queue, pPresentInfo);
                        reportDeviceLostDiagnostics(passThroughDevice.get(), queue, "vkQueuePresentKHR(toast fallback)", passthroughResult);
                        if (passthroughResult == VK_ERROR_DEVICE_LOST)
                            panicLayer(passThroughDevice.get(), "Vulkan device lost during toast fallback present");
                        return passthroughResult;
                    }
                    presentWaitSems.push_back(swap->overlaySemaphores[index]);
                }

                VkPresentInfoKHR pi = *pPresentInfo;
                pi.waitSemaphoreCount = static_cast<uint32_t>(presentWaitSems.size());
                pi.pWaitSemaphores = presentWaitSems.data();
                VkResult vr = passThroughDevice->vkd.QueuePresentKHR(queue, &pi);
                reportDeviceLostDiagnostics(passThroughDevice.get(), queue, "vkQueuePresentKHR(toast overlay)", vr);
                if (vr == VK_ERROR_DEVICE_LOST)
                    panicLayer(passThroughDevice.get(), "Vulkan device lost during toast overlay present");
                return vr;
            }
        }

        if (!isWayland() && !isX11())
        {
            std::shared_ptr<LogicalDevice> passThroughDevice;

            {
                scoped_lock l(globalLock);

                auto devIt = deviceMap.find(GetKey(queue));
                if (devIt == deviceMap.end() || !devIt->second)
                    return VK_ERROR_DEVICE_LOST;
                if (!devIt->second->queue)
                {
                    VkResult vr = devIt->second->vkd.QueuePresentKHR(queue, pPresentInfo);
                    reportDeviceLostDiagnostics(devIt->second.get(), queue, "vkQueuePresentKHR(unsupported-surface direct)", vr);
                    if (vr == VK_ERROR_DEVICE_LOST)
                        panicLayer(devIt->second.get(), "Vulkan device lost during unsupported-surface direct present");
                    return vr;
                }

                passThroughDevice = devIt->second;
            }

            VkResult vr = passThroughDevice->vkd.QueuePresentKHR(queue, pPresentInfo);
            reportDeviceLostDiagnostics(passThroughDevice.get(), queue, "vkQueuePresentKHR(unsupported-surface passthrough)", vr);
            if (vr == VK_ERROR_DEVICE_LOST)
                panicLayer(passThroughDevice.get(), "Vulkan device lost during unsupported-surface passthrough present");
            return vr;
        }

        // Mark new input frame so dispatch deduplication resets
        if (isWayland())
            beginWaylandInputFrame();
        beginKeyboardInputFrame();

        // Keybindings - read from settingsManager (can be updated when settings are saved)
        static uint32_t keySymbol = convertToKeySym(settingsManager.getToggleKey());
        static uint32_t reloadKeySymbol = convertToKeySym(settingsManager.getReloadKey());
        static uint32_t overlayKeySymbol = convertToKeySym(settingsManager.getOverlayKey());
        static bool initLogged = false;

        static bool pressed       = false;
        static bool presentEffect = settingsManager.getEnableOnLaunch();
        static bool reloadPressed = false;
        static bool overlayPressed = false;

        std::shared_ptr<LogicalDevice> logicalDeviceShared;
        bool presentEffectSnapshot = false;
        std::vector<std::shared_ptr<LogicalSwapchain>> presentSwapchains;
        std::vector<uint32_t> presentIndices;

        // REMOVED: No longer need rawDeviceForRealloc/queueForRealloc since we removed QueueWaitIdle

        {
            scoped_lock l(globalLock);

            // Guard: if no device for this queue, pass through
            auto devIt = deviceMap.find(GetKey(queue));
            if (devIt == deviceMap.end() || !devIt->second)
                return VK_ERROR_DEVICE_LOST;
            if (!devIt->second->queue)
                return devIt->second->vkd.QueuePresentKHR(queue, pPresentInfo);

            LogicalDevice* deviceForSettings = devIt->second.get();

            // Check if settings were saved (re-read from settingsManager which is already updated by UI)
            if (deviceForSettings && deviceForSettings->imguiOverlay && deviceForSettings->imguiOverlay->hasSettingsSaved())
            {
                // settingsManager is already updated by the UI, just re-read the values
                keySymbol = convertToKeySym(settingsManager.getToggleKey());
                reloadKeySymbol = convertToKeySym(settingsManager.getReloadKey());
                overlayKeySymbol = convertToKeySym(settingsManager.getOverlayKey());
                initInputBlocker(settingsManager.getOverlayBlockInput());
                if (deviceForSettings->imguiOverlay)
                    setInputBlocked(deviceForSettings->imguiOverlay->isVisible());
                deviceForSettings->imguiOverlay->clearSettingsSaved();
                Logger::info("Settings reloaded from SettingsManager");
            }

            // Check if shader paths were changed (refresh available effects list)
            if (deviceForSettings && deviceForSettings->imguiOverlay && deviceForSettings->imguiOverlay->hasShaderPathsChanged())
            {
                cachedEffects.initialized = false;  // Force re-scan of available effects
                deviceForSettings->imguiOverlay->clearShaderPathsChanged();
                Logger::info("Shader paths changed, effect list refreshed");
            }

            if (!initLogged)
            {
                Logger::info("hot-reload initialized, config: " + config->getConfigFilePath());
                initLogged = true;
            }

            // REMOVED: Deferred startup reload was causing race conditions during graphics quality switches

            // Toggle effect on/off (keyboard)
            if (handleKeyPress(keySymbol, pressed))
                presentEffect = !presentEffect;

            // Hot-reload: check for key press or config file change
            bool shouldReload = false;
            bool reloadFromDisk = false;
            if (handleKeyPress(reloadKeySymbol, reloadPressed))
            {
                Logger::debug("reload key pressed");
                shouldReload = true;
                reloadFromDisk = true;
            }
            if (config->hasConfigChanged())
            {
                Logger::debug("config file changed detected");
                shouldReload = true;
                reloadFromDisk = true;
            }

            // Toggle overlay on/off
            if (handleKeyPress(overlayKeySymbol, overlayPressed))
            {
                if (deviceForSettings->imguiOverlay)
                    deviceForSettings->imguiOverlay->toggle();
            }

            // Check for Apply button press in overlay (overlay is at device level)
            LogicalDevice* logicalDevice = deviceForSettings;

            // Toggle effects on/off via overlay checkbox
            if (logicalDevice->imguiOverlay && logicalDevice->imguiOverlay->hasToggleEffectsRequest())
            {
                presentEffect = !presentEffect;
                logicalDevice->imguiOverlay->clearToggleEffectsRequest();
            }

            // Depth pin changed — trigger reload so command buffers pick up the new depth
            if (logicalDevice->imguiOverlay && logicalDevice->imguiOverlay->hasDepthPinChanged())
            {
                logicalDevice->imguiOverlay->clearDepthPinChanged();
                shouldReload = true;
            }

            if (logicalDevice->imguiOverlay && logicalDevice->imguiOverlay->hasModifiedParams())
            {
                // Modified parameters live in EffectRegistry — effects pick
                // them up at reload time, so all we need to do here is clear
                // the request and trigger a reload.
                logicalDevice->imguiOverlay->clearApplyRequest();
                shouldReload = true;
            }

            if (shouldReload)
            {
                if (logicalDevice->imguiOverlay)
                    logicalDevice->imguiOverlay->refreshShaderProfiles();
                Logger::info("hot-reloading config and effects...");
                auto reloadSelectedEffects = [&]() {
                    cachedEffects.initialized = false;
                    cachedParams.dirty = true;
                    const std::vector<std::string> activeEffects = logicalDevice->imguiOverlay
                        ? logicalDevice->imguiOverlay->getActiveEffects()
                        : config->getOption<std::vector<std::string>>("effects", {});
                    reloadAllSwapchains(logicalDevice, activeEffects);
                    logicalDevice->depthReallocPending = false;
                };

                // the overlay owns the active profile; the static is only for
                // early calls before it exists.
                std::string overlayShaderPath = logicalDevice->imguiOverlay
                    ? logicalDevice->imguiOverlay->getActiveShaderProfilePath()
                    : std::string();
                if (overlayShaderPath.empty())
                    overlayShaderPath = activeShaderProfilePath;

                // Check if overlay wants to load a different config
                if (logicalDevice->imguiOverlay && logicalDevice->imguiOverlay->hasPendingConfig())
                {
                    std::string newConfigPath = logicalDevice->imguiOverlay->getPendingConfigPath();
                    switchConfig(newConfigPath, overlayShaderPath);
                    // Update overlay with effects from the new config
                    std::vector<std::string> newEffects = config->getOption<std::vector<std::string>>("effects", {});
                    std::vector<std::string> disabledEffects = config->getOption<std::vector<std::string>>("disabledEffects", {});
                    logicalDevice->imguiOverlay->setSelectedEffects(newEffects, disabledEffects);
                    logicalDevice->imguiOverlay->clearPendingConfig();
                    reloadSelectedEffects();
                }
                else if (logicalDevice->imguiOverlay && logicalDevice->imguiOverlay->hasPendingShaderProfile())
                {
                    const std::string shaderPath = logicalDevice->imguiOverlay->getPendingShaderProfilePath();
                    if (!shaderPath.empty())
                    {
                        switchConfig(config->getConfigFilePath(), shaderPath);
                        std::vector<std::string> newEffects = config->getOption<std::vector<std::string>>("effects", {});
                        std::vector<std::string> disabledEffects = config->getOption<std::vector<std::string>>("disabledEffects", {});
                        logicalDevice->imguiOverlay->setSelectedEffects(newEffects, disabledEffects);
                    }
                    else
                    {
                        switchConfig(config->getConfigFilePath(), "");
                        const std::vector<std::string> newEffects = config->getOption<std::vector<std::string>>("effects", {});
                        const std::vector<std::string> disabledEffects = config->getOption<std::vector<std::string>>("disabledEffects", {});
                        logicalDevice->imguiOverlay->setSelectedEffects(newEffects, disabledEffects);
                    }
                    logicalDevice->imguiOverlay->clearPendingShaderProfile();
                    reloadSelectedEffects();
                }
                else
                {
                    // only the reload key or an external file change re-reads the
                    // config and re-applies the profile; a UI apply rebuilds the
                    // chain from the registry, which is already the truth
                    if (reloadFromDisk)
                    {
                        config->reload();
                        applyShaderProfile(config.get(), overlayShaderPath);
                    }
                    reloadSelectedEffects();
                }
            }

            // Check for debounced resize reload (separate from config reload)
            // Only call steady_clock::now() when a resize is actually pending
            if (resizeDebounce.pending)
            {
                auto resizeElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - resizeDebounce.lastResizeTime).count();

                if (resizeElapsed >= RESIZE_DEBOUNCE_MS)
                {
                    Logger::info("debounced resize reload after " + std::to_string(resizeElapsed) + "ms");
                    resizeDebounce.pending = false;

                    // Get selected effects from registry (single source of truth)
                    const auto& selectedEffects = effectRegistry.getSelectedEffects();
                    for (auto& [_, swapchain] : swapchainMap)
                    {
                        if (swapchain->fakeImages.empty())
                            continue;
                        reloadEffectsForSwapchain(swapchain.get(), config.get(), selectedEffects);
                    }
                    logicalDevice->depthReallocPending = false;
                }
            }

            // Keep lock scope small: snapshot pointers and immutable per-present state,
            // then do command submission and present outside the global mutex.
            updateOverlayState(logicalDevice, presentEffect);
            presentEffectSnapshot = presentEffect;
            logicalDeviceShared = devIt->second;

            presentSwapchains.reserve(pPresentInfo->swapchainCount);
            presentIndices.reserve(pPresentInfo->swapchainCount);
            for (unsigned int i = 0; i < pPresentInfo->swapchainCount; i++)
            {
                auto swapIt = swapchainMap.find(pPresentInfo->pSwapchains[i]);
                if (swapIt == swapchainMap.end() || !swapIt->second)
                {
                    Logger::err("present references unknown swapchain");
                    return VK_ERROR_OUT_OF_DATE_KHR;
                }

                LogicalSwapchain* logicalSwapchain = swapIt->second.get();
                uint32_t index = pPresentInfo->pImageIndices[i];
                if (index >= logicalSwapchain->imageCount
                    || index >= logicalSwapchain->semaphores.size()
                    || index >= logicalSwapchain->overlaySemaphores.size()
                    || index >= logicalSwapchain->imageViews.size())
                {
                    Logger::err("present image index out of bounds for swapchain");
                    return VK_ERROR_OUT_OF_DATE_KHR;
                }

                const auto& commandBuffers = presentEffectSnapshot
                    ? logicalSwapchain->commandBuffersEffect
                    : logicalSwapchain->commandBuffersNoEffect;
                if (index >= commandBuffers.size())
                {
                    Logger::err("present command buffer index out of bounds");
                    return VK_ERROR_OUT_OF_DATE_KHR;
                }

                presentSwapchains.push_back(swapIt->second);
                presentIndices.push_back(index);
            }
        }

        LogicalDevice* logicalDevice = logicalDeviceShared.get();
        if (!logicalDevice)
            return VK_ERROR_DEVICE_LOST;

        // Depth recovery is intentionally non-blocking.  If the currently
        // selected depth image does not exactly match the present swapchain,
        // do not submit our existing effect command buffer: it may reference
        // a depth image that the game has just resized/destroyed.  Pass the
        // application's present straight through while depth is disabled.
        bool bypassDepthForPresent = false;
        {
            scoped_lock depthLock(globalLock);

            // Depth promotion and pin changes happen while the application is
            // recording/submitting its render work. Rebuild our command
            // buffers here, at the next present boundary, after every affected
            // swapchain's previous effect submissions have completed. Without
            // consuming this flag, newly discovered depth is captured but the
            // effects keep using command buffers recorded without depth until
            // a full config reload (F10/Delete) occurs.
            if (logicalDevice->depthReallocPending)
            {
                for (const auto& [_, sc] : swapchainMap)
                    if (sc && sc->logicalDevice == logicalDevice)
                        sc->depthReallocPending = true;
                logicalDevice->depthReallocPending = false;
            }

            DepthState effectiveDepth = getDepthState(logicalDevice);
            if (hasDepthState(effectiveDepth) &&
                !validateDepthStateForResolve(logicalDevice, effectiveDepth))
                effectiveDepth = {};

            for (const auto& scPtr : presentSwapchains)
            {
                LogicalSwapchain* sc = scPtr.get();
                if (!sc || sc->logicalDevice != logicalDevice || !sc->depthReallocPending
                    || sc->commandBuffersEffect.empty())
                    continue;

                if (!depthRebuildFencesReady(logicalDevice, sc))
                {
                    // Keep the request armed and pass through until the last
                    // submission using this swapchain's buffers has completed.
                    bypassDepthForPresent = true;
                    continue;
                }

                DepthState swapchainDepth{};
                if (depthMatchesSwapchainExtent(effectiveDepth, sc))
                    swapchainDepth = effectiveDepth;
                else if (logicalDevice->pinnedDepthImageView == VK_NULL_HANDLE)
                    selectDepthCandidateForSwapchainLocked(logicalDevice, sc, swapchainDepth);

                if (!hasDepthState(effectiveDepth) && hasDepthState(swapchainDepth))
                {
                    effectiveDepth = swapchainDepth;
                    logicalDevice->activeDepthState = swapchainDepth;
                }

                reallocateCommandBuffers(logicalDevice, sc, swapchainDepth);
                sc->depthReallocPending = false;
                Logger::debug(std::string("deferred depth change rebuilt presented swapchain")
                              + (hasDepthState(swapchainDepth) ? " with depth " : " without depth ")
                              + std::to_string(swapchainDepth.extent.width) + "x"
                              + std::to_string(swapchainDepth.extent.height));
            }

            for (const auto& scPtr : presentSwapchains)
            {
                LogicalSwapchain* sc = scPtr.get();
                if (!sc || sc->logicalDevice != logicalDevice)
                    continue;

                DepthState currentDepth = getDepthState(logicalDevice);
                const bool currentValid = !hasDepthState(currentDepth)
                    || validateDepthStateForResolve(logicalDevice, currentDepth);
                const bool exactMatch = currentValid
                    && depthMatchesSwapchainExtent(currentDepth, sc);

                auto retryIt = depthRetryStates.find(logicalDevice);
                const bool retryDisabled = retryIt != depthRetryStates.end() && retryIt->second.disabled;

                if (hasDepthState(currentDepth) && (!currentValid || !exactMatch))
                {
                    armDepthRetryLocked(logicalDevice, sc,
                                        currentValid ? "depth extent does not match swapchain"
                                                     : "depth image is no longer valid");
                    bypassDepthForPresent = true;
                    break;
                }

                if (retryDisabled)
                {
                    bypassDepthForPresent = true;

                    if (depthRetryDueLocked(logicalDevice))
                    {
                        DepthState candidate{};
                        if (!selectDepthCandidateForSwapchainLocked(logicalDevice, sc, candidate))
                            candidate = getDepthState(logicalDevice);
                        if (depthMatchesSwapchainExtent(candidate, sc)
                            && validateDepthStateForResolve(logicalDevice, candidate))
                        {
                            if (depthRebuildFencesReady(logicalDevice, sc))
                            {
                                logicalDevice->activeDepthState = candidate;
                                reallocateCommandBuffers(logicalDevice, sc, candidate);
                                logicalDevice->depthReallocPending = false;
                                for (auto& [_, swapchain] : swapchainMap)
                                    if (swapchain && swapchain->logicalDevice == logicalDevice
                                        && swapchain.get() != sc)
                                        swapchain->depthReallocPending = true;
                                auto& retry = depthRetryStates[logicalDevice];
                                retry.disabled = false;
                                retry.retryPending = false;
                                retry.retryAt = {};
                                bypassDepthForPresent = false;
                            }
                            else
                                scheduleDepthRetryLocked(logicalDevice, true);
                        }
                        else
                        {
                            Logger::debug("depth retry: no exact swapchain-sized depth buffer yet");
                            scheduleDepthRetryLocked(logicalDevice, false);
                        }
                    }
                    break;
                }
            }

        }

        if (bypassDepthForPresent)
        {
            // Crucially, don't submit the old command buffer while depth is
            // invalid.  This is what prevents a resize race from escalating
            // into VK_ERROR_DEVICE_LOST.  The app's own present remains intact.
            VkResult passthroughResult = logicalDevice->vkd.QueuePresentKHR(queue, pPresentInfo);
            reportDeviceLostDiagnostics(logicalDevice, queue, "vkQueuePresentKHR(depth-disabled passthrough)", passthroughResult);
            if (passthroughResult == VK_ERROR_DEVICE_LOST)
                panicLayer(logicalDevice, "Vulkan device lost during depth-disabled passthrough present");
            return passthroughResult;
        }

        // Reuse static buffers to avoid per-frame heap allocations
        static thread_local std::vector<VkSemaphore> presentSemaphores;
        static thread_local std::vector<VkPipelineStageFlags> waitStages;
        presentSemaphores.clear();
        presentSemaphores.reserve(pPresentInfo->swapchainCount);
        waitStages.assign(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

        for (unsigned int i = 0; i < pPresentInfo->swapchainCount; i++)
        {
            LogicalSwapchain* logicalSwapchain = presentSwapchains[i].get();
            uint32_t index = presentIndices[i];

            // Update effect uniforms only when effects are active (saves CPU+GPU when off).
            // Wrapped in try/catch so a misbehaving effect's per-frame update can
            // never escape into vkQueuePresentKHR (which is noexcept from the
            // app's perspective).
            if (presentEffectSnapshot)
            {
                try
                {
                    for (auto& effect : logicalSwapchain->effects)
                        effect->updateEffect();
                }
                catch (const std::exception& e)
                {
                    panicLayer(logicalDevice,
                        std::string("Effect update threw: ") + e.what());
                    return logicalDevice->vkd.QueuePresentKHR(queue, pPresentInfo);
                }
                catch (...)
                {
                    panicLayer(logicalDevice, "Effect update threw unknown exception");
                    return logicalDevice->vkd.QueuePresentKHR(queue, pPresentInfo);
                }
            }

            const auto& commandBuffers = presentEffectSnapshot
                ? logicalSwapchain->commandBuffersEffect
                : logicalSwapchain->commandBuffersNoEffect;

            // Submit effect command buffer
            VkSubmitInfo submitInfo = {};
            submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.waitSemaphoreCount = i == 0 ? pPresentInfo->waitSemaphoreCount : 0;
            submitInfo.pWaitSemaphores    = i == 0 ? pPresentInfo->pWaitSemaphores : nullptr;
            submitInfo.pWaitDstStageMask  = i == 0 ? waitStages.data() : nullptr;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers    = &commandBuffers[index];
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores    = &logicalSwapchain->semaphores[index];

            // Signal this image's fence so we can safely update its
            // descriptor sets and rebuild its command buffer in future frames.
            VkFence effectFence = (index < logicalSwapchain->effectSubmitFences.size())
                ? logicalSwapchain->effectSubmitFences[index] : VK_NULL_HANDLE;
            VkResult vr = logicalDevice->vkd.QueueSubmit(logicalDevice->queue, 1, &submitInfo, effectFence);
            if (vr != VK_SUCCESS)
            {
                reportDeviceLostDiagnostics(logicalDevice, logicalDevice->queue, "vkQueueSubmit(effect)", vr);
                if (vr == VK_ERROR_DEVICE_LOST)
                    panicLayer(logicalDevice, "Vulkan device lost during effect submit");
                return vr;
            }
            if (index < logicalSwapchain->effectSubmitFenceUsed.size())
                logicalSwapchain->effectSubmitFenceUsed[index] = true;

            maybeDumpDepthResolveImage(logicalDevice, logicalSwapchain, index, logicalDevice->queue);

            VkSemaphore finalSemaphore;
            try
            {
                vr = submitOverlayFrame(logicalDevice, logicalSwapchain, index, finalSemaphore);
            }
            catch (const std::exception& e)
            {
                panicLayer(logicalDevice, std::string("Overlay submit threw: ") + e.what());
                return logicalDevice->vkd.QueuePresentKHR(queue, pPresentInfo);
            }
            catch (...)
            {
                panicLayer(logicalDevice, "Overlay submit threw unknown exception");
                return logicalDevice->vkd.QueuePresentKHR(queue, pPresentInfo);
            }
            if (vr != VK_SUCCESS)
            {
                reportDeviceLostDiagnostics(logicalDevice, logicalDevice->queue, "submitOverlayFrame", vr);
                if (vr == VK_ERROR_DEVICE_LOST)
                    panicLayer(logicalDevice, "Vulkan device lost during overlay submit");
                return vr;
            }

            presentSemaphores.push_back(finalSemaphore);
        }

        VkPresentInfoKHR presentInfo   = *pPresentInfo;
        presentInfo.waitSemaphoreCount = presentSemaphores.size();
        presentInfo.pWaitSemaphores    = presentSemaphores.data();

        VkResult presentResult = logicalDevice->vkd.QueuePresentKHR(queue, &presentInfo);
        reportDeviceLostDiagnostics(logicalDevice, queue, "vkQueuePresentKHR(final)", presentResult);
        if (presentResult == VK_ERROR_DEVICE_LOST)
            panicLayer(logicalDevice, "Vulkan device lost during final present");
        
        // OUT_OF_DATE / SUBOPTIMAL: flag the affected swapchains for reset.
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
        {
            Logger::warn("QueuePresentKHR returned " + std::string(presentResult == VK_ERROR_OUT_OF_DATE_KHR ? "OUT_OF_DATE" : "SUBOPTIMAL") + ", flagging swapchains for reset");
            scoped_lock l(globalLock);
            for (unsigned int i = 0; i < pPresentInfo->swapchainCount; i++)
            {
                auto swapIt = swapchainMap.find(pPresentInfo->pSwapchains[i]);
                if (swapIt != swapchainMap.end() && swapIt->second)
                {
                    LogicalSwapchain* sc = swapIt->second.get();
                    // Reset depth resolve state to prevent use of stale resources
                    sc->depthResolveSourceView = VK_NULL_HANDLE;
                    
                    for (auto& perImg : sc->depthResolvePerImage) perImg.image = VK_NULL_HANDLE;
                    // Flag for reload on next valid frame
                    sc->rebuildEffectsOnNextPresent = true;
                }
            }
            // Don't return error - let app handle the resize/recreate naturally
            return VK_SUCCESS;
        }
        
        return presentResult;
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
    {
        if (!swapchain)
            return;

        scoped_lock l(globalLock);
        // we need to delete the infos of the oldswapchain

        Logger::trace("vkDestroySwapchainKHR " + convertToString(swapchain));
        {
            auto it = swapchainMap.find(swapchain);
            if (it != swapchainMap.end() && it->second)
                it->second->destroy();
            swapchainMap.erase(swapchain);
        }
        auto devIt = deviceMap.find(GetKey(device));
        if (devIt != deviceMap.end() && devIt->second)
            devIt->second->vkd.DestroySwapchainKHR(device, swapchain, pAllocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VKIntox_CreateRenderPass(VkDevice device,
                                                            const VkRenderPassCreateInfo* pCreateInfo,
                                                            const VkAllocationCallbacks* pAllocator,
                                                            VkRenderPass* pRenderPass)
    {
        scoped_lock l(globalLock);
        LogicalDevice* logicalDevice = deviceMap[GetKey(device)].get();
        if (logicalDevice == nullptr)
            return VK_ERROR_DEVICE_LOST;
        VkResult vr;
        if (!VKIntox::settingsManager.getDepthCapture() || !pCreateInfo || pCreateInfo->attachmentCount == 0)
        {
            vr = logicalDevice->vkd.CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);
            if (vr == VK_SUCCESS && pRenderPass)
            {
                for (uint32_t ai = 0; ai < pCreateInfo->attachmentCount; ai++)
                {
                    if (isDepthFormat(pCreateInfo->pAttachments[ai].format))
                    {
                        renderPassDepthFinalLayouts[*pRenderPass] = pCreateInfo->pAttachments[ai].finalLayout;
                        break;
                    }
                }
            }
            return vr;
        }

        std::vector<VkAttachmentDescription> attachments(
            pCreateInfo->pAttachments,
            pCreateInfo->pAttachments + pCreateInfo->attachmentCount);

        bool changed = false;
        for (auto& attachment : attachments)
            changed = forceDepthAttachmentStoreOp(attachment) || changed;

        if (!changed)
        {
            vr = logicalDevice->vkd.CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);
            if (vr == VK_SUCCESS && pRenderPass)
            {
                for (uint32_t ai = 0; ai < pCreateInfo->attachmentCount; ai++)
                {
                    if (isDepthFormat(pCreateInfo->pAttachments[ai].format))
                    {
                        renderPassDepthFinalLayouts[*pRenderPass] = pCreateInfo->pAttachments[ai].finalLayout;
                        break;
                    }
                }
            }
            return vr;
        }

        VkRenderPassCreateInfo createInfo = *pCreateInfo;
        createInfo.pAttachments = attachments.data();
        Logger::debug("forcing depth attachment storeOp=STORE for VkRenderPassCreateInfo with attachmentCount="
                      + std::to_string(createInfo.attachmentCount));
        vr = logicalDevice->vkd.CreateRenderPass(device, &createInfo, pAllocator, pRenderPass);
        if (vr == VK_SUCCESS && pRenderPass)
        {
            for (uint32_t ai = 0; ai < createInfo.attachmentCount; ai++)
            {
                if (isDepthFormat(createInfo.pAttachments[ai].format))
                {
                    renderPassDepthFinalLayouts[*pRenderPass] = createInfo.pAttachments[ai].finalLayout;
                    break;
                }
            }
        }
        return vr;
    }

    VKAPI_ATTR VkResult VKAPI_CALL VKIntox_CreateRenderPass2(VkDevice device,
                                                             const VkRenderPassCreateInfo2* pCreateInfo,
                                                             const VkAllocationCallbacks* pAllocator,
                                                             VkRenderPass* pRenderPass)
    {
        scoped_lock l(globalLock);
        LogicalDevice* logicalDevice = deviceMap[GetKey(device)].get();
        if (logicalDevice == nullptr)
            return VK_ERROR_DEVICE_LOST;
        VkResult vr;
        if (!VKIntox::settingsManager.getDepthCapture() || !pCreateInfo || pCreateInfo->attachmentCount == 0)
        {
            vr = logicalDevice->vkd.CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);
            if (vr == VK_SUCCESS && pRenderPass)
            {
                for (uint32_t ai = 0; ai < pCreateInfo->attachmentCount; ai++)
                {
                    if (isDepthFormat(pCreateInfo->pAttachments[ai].format))
                    {
                        renderPassDepthFinalLayouts[*pRenderPass] = pCreateInfo->pAttachments[ai].finalLayout;
                        break;
                    }
                }
            }
            return vr;
        }

        std::vector<VkAttachmentDescription2> attachments(
            pCreateInfo->pAttachments,
            pCreateInfo->pAttachments + pCreateInfo->attachmentCount);

        bool changed = false;
        for (auto& attachment : attachments)
            changed = forceDepthAttachmentStoreOp(attachment) || changed;

        if (!changed)
        {
            vr = logicalDevice->vkd.CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);
            if (vr == VK_SUCCESS && pRenderPass)
            {
                for (uint32_t ai = 0; ai < pCreateInfo->attachmentCount; ai++)
                {
                    if (isDepthFormat(pCreateInfo->pAttachments[ai].format))
                    {
                        renderPassDepthFinalLayouts[*pRenderPass] = pCreateInfo->pAttachments[ai].finalLayout;
                        break;
                    }
                }
            }
            return vr;
        }

        VkRenderPassCreateInfo2 createInfo = *pCreateInfo;
        createInfo.pAttachments = attachments.data();
        Logger::debug("forcing depth attachment storeOp=STORE for VkRenderPassCreateInfo2 with attachmentCount="
                      + std::to_string(createInfo.attachmentCount));
        vr = logicalDevice->vkd.CreateRenderPass2(device, &createInfo, pAllocator, pRenderPass);
        if (vr == VK_SUCCESS && pRenderPass)
        {
            for (uint32_t ai = 0; ai < createInfo.attachmentCount; ai++)
            {
                if (isDepthFormat(createInfo.pAttachments[ai].format))
                {
                    renderPassDepthFinalLayouts[*pRenderPass] = createInfo.pAttachments[ai].finalLayout;
                    break;
                }
            }
        }
        return vr;
    }

    VKAPI_ATTR VkResult VKAPI_CALL VKIntox_CreateRenderPass2KHR(VkDevice device,
                                                                const VkRenderPassCreateInfo2* pCreateInfo,
                                                                const VkAllocationCallbacks* pAllocator,
                                                                VkRenderPass* pRenderPass)
    {
        scoped_lock l(globalLock);
        LogicalDevice* logicalDevice = deviceMap[GetKey(device)].get();
        if (logicalDevice == nullptr)
            return VK_ERROR_DEVICE_LOST;
        VkResult vr;
        if (!VKIntox::settingsManager.getDepthCapture() || !pCreateInfo || pCreateInfo->attachmentCount == 0)
        {
            vr = logicalDevice->vkd.CreateRenderPass2KHR(device, pCreateInfo, pAllocator, pRenderPass);
            if (vr == VK_SUCCESS && pRenderPass)
            {
                for (uint32_t ai = 0; ai < pCreateInfo->attachmentCount; ai++)
                {
                    if (isDepthFormat(pCreateInfo->pAttachments[ai].format))
                    {
                        renderPassDepthFinalLayouts[*pRenderPass] = pCreateInfo->pAttachments[ai].finalLayout;
                        break;
                    }
                }
            }
            return vr;
        }

        std::vector<VkAttachmentDescription2> attachments(
            pCreateInfo->pAttachments,
            pCreateInfo->pAttachments + pCreateInfo->attachmentCount);

        bool changed = false;
        for (auto& attachment : attachments)
            changed = forceDepthAttachmentStoreOp(attachment) || changed;

        if (!changed)
        {
            vr = logicalDevice->vkd.CreateRenderPass2KHR(device, pCreateInfo, pAllocator, pRenderPass);
            if (vr == VK_SUCCESS && pRenderPass)
            {
                for (uint32_t ai = 0; ai < pCreateInfo->attachmentCount; ai++)
                {
                    if (isDepthFormat(pCreateInfo->pAttachments[ai].format))
                    {
                        renderPassDepthFinalLayouts[*pRenderPass] = pCreateInfo->pAttachments[ai].finalLayout;
                        break;
                    }
                }
            }
            return vr;
        }

        VkRenderPassCreateInfo2 createInfo = *pCreateInfo;
        createInfo.pAttachments = attachments.data();
        Logger::debug("forcing depth attachment storeOp=STORE for VkRenderPassCreateInfo2KHR with attachmentCount="
                      + std::to_string(createInfo.attachmentCount));
        vr = logicalDevice->vkd.CreateRenderPass2KHR(device, &createInfo, pAllocator, pRenderPass);
        if (vr == VK_SUCCESS && pRenderPass)
        {
            for (uint32_t ai = 0; ai < createInfo.attachmentCount; ai++)
            {
                if (isDepthFormat(createInfo.pAttachments[ai].format))
                {
                    renderPassDepthFinalLayouts[*pRenderPass] = createInfo.pAttachments[ai].finalLayout;
                    break;
                }
            }
        }
        return vr;
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_DestroyRenderPass(VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks* pAllocator)
    {
        scoped_lock l(globalLock);
        renderPassDepthFinalLayouts.erase(renderPass);
        if (!device)
            return;
        auto devIt = deviceMap.find(GetKey(device));
        if (devIt != deviceMap.end() && devIt->second)
            devIt->second->vkd.DestroyRenderPass(device, renderPass, pAllocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VKIntox_CreateImage(VkDevice                     device,
                                                       const VkImageCreateInfo*     pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator,
                                                       VkImage*                     pImage)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            LogicalDevice* logicalDevice = deviceMap[GetKey(device)].get();
            if (logicalDevice == nullptr)
                return VK_ERROR_DEVICE_LOST;
            return logicalDevice->vkd.CreateImage(device, pCreateInfo, pAllocator, pImage);
        }

        scoped_lock l(globalLock);

        LogicalDevice* logicalDevice = deviceMap[GetKey(device)].get();
        if (logicalDevice == nullptr)
            return VK_ERROR_DEVICE_LOST;
        if (isDepthFormat(pCreateInfo->format)
            && ((pCreateInfo->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
        {
            Logger::debug("detected depth image with format: " + convertToString(pCreateInfo->format));
            Logger::debug(std::to_string(pCreateInfo->extent.width) + "x" + std::to_string(pCreateInfo->extent.height));
            Logger::debug("samples: " + convertToString(pCreateInfo->samples));
            Logger::debug(
                std::to_string((pCreateInfo->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));

            VkImageCreateInfo modifiedCreateInfo = *pCreateInfo;
            // resolve needs TRANSFER_SRC, the shader fallback needs SAMPLED.
            //
            // TRANSIENT can't coexist with either (VUID-VkImageCreateInfo-usage-00963).
            // mobile apps set it for lazy allocation, and on tiled renderers the
            // image then has no backing outside the app's pass — sampling it gives
            // garbage. that's why depth was blank in roblox while colour postproc
            // worked. strip it so the image is sampleable.
            if (modifiedCreateInfo.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)
            {
                modifiedCreateInfo.usage &= ~VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
                Logger::info("stripped VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT from depth image "
                             "(format=" + convertToString(pCreateInfo->format)
                             + ", samples=" + convertToString(pCreateInfo->samples)
                             + ") — required for layer-side sampling/resolve");
            }
            modifiedCreateInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

            // Metadata reflects the *actual* usage flags passed to the driver
            // (post-modification), so downstream code can correctly reason about
            // what the image can do.
            DepthImageMetadata metadata = {
                modifiedCreateInfo.usage,
                pCreateInfo->samples,
                pCreateInfo->tiling,
            };

            VkResult result = logicalDevice->vkd.CreateImage(device, &modifiedCreateInfo, pAllocator, pImage);
            if (result != VK_SUCCESS)
                return result;

            logicalDevice->depthImages.push_back(*pImage);
            logicalDevice->depthFormats.push_back(pCreateInfo->format);
            logicalDevice->depthImageExtents[*pImage] = pCreateInfo->extent;
            logicalDevice->depthImageMetadata[*pImage] = metadata;
            Logger::debug("tracked depth image metadata: image=" + convertToString(*pImage)
                          + " usage=0x" + formatHexU64(static_cast<uint64_t>(metadata.usage))
                          + " samples=" + convertToString(metadata.samples)
                          + " tiling=" + convertToString(metadata.tiling)
                          + " transient=" + std::string((metadata.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0 ? "true" : "false"));

            return result;
        }
        else
        {
            return logicalDevice->vkd.CreateImage(device, pCreateInfo, pAllocator, pImage);
        }
    }

    VKAPI_ATTR VkResult VKAPI_CALL VKIntox_BindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            LogicalDevice* logicalDevice = deviceMap[GetKey(device)].get();
            if (logicalDevice == nullptr)
                return VK_ERROR_DEVICE_LOST;
            return logicalDevice->vkd.BindImageMemory(device, image, memory, memoryOffset);
        }

        scoped_lock l(globalLock);

        LogicalDevice* logicalDevice = deviceMap[GetKey(device)].get();
        if (logicalDevice == nullptr)
            return VK_ERROR_DEVICE_LOST;
        // No layer bookkeeping needed here — depth image metadata is populated
        // at CreateImage time and used at CmdBeginRenderPass time. The previous
        // implementation did a std::find and then returned `result` either way.
        return logicalDevice->vkd.BindImageMemory(device, image, memory, memoryOffset);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VKIntox_CreateImageView(VkDevice device,
                                                           const VkImageViewCreateInfo* pCreateInfo,
                                                           const VkAllocationCallbacks* pAllocator,
                                                           VkImageView* view)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            LogicalDevice* logicalDevice = deviceMap[GetKey(device)].get();
            if (logicalDevice == nullptr)
                return VK_ERROR_DEVICE_LOST;
            return logicalDevice->vkd.CreateImageView(device, pCreateInfo, pAllocator, view);
        }

        scoped_lock l(globalLock);

        LogicalDevice* logicalDevice = deviceMap[GetKey(device)].get();
        if (logicalDevice == nullptr)
            return VK_ERROR_DEVICE_LOST;
        VkResult result = logicalDevice->vkd.CreateImageView(device, pCreateInfo, pAllocator, view);
        if (result != VK_SUCCESS)
            return result;

        DepthSnapshotTarget snapshotTarget = selectDepthSnapshotTargetFromImage(logicalDevice, pCreateInfo->image);
        if (snapshotTarget.swapchain != VK_NULL_HANDLE)
        {
            Logger::debug("tracked snapshot target image view: appView=" + convertToString(*view)
                          + " image=" + convertToString(pCreateInfo->image)
                          + " swapchain=" + convertToString(snapshotTarget.swapchain)
                          + " imageIndex=" + std::to_string(snapshotTarget.imageIndex));
            logicalDevice->snapshotTargetViewStates[*view] = snapshotTarget;
        }

        if ((pCreateInfo->subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) == 0)
            return result;

        auto imageIt = std::find(logicalDevice->depthImages.begin(), logicalDevice->depthImages.end(), pCreateInfo->image);
        if (imageIt == logicalDevice->depthImages.end())
            return result;

        size_t i = std::distance(logicalDevice->depthImages.begin(), imageIt);
        // Bounds-check the parallel vectors before indexing.
        if (i >= logicalDevice->depthFormats.size())
        {
            Logger::warn("CreateImageView: depth image index " + std::to_string(i)
                         + " out of depthFormats range (" + std::to_string(logicalDevice->depthFormats.size())
                         + "); skipping depth view tracking for image="
                         + convertToString(pCreateInfo->image));
            return result;
        }
        VkImageView sampledView = getOrCreateTrackedDepthSampleViewLocked(logicalDevice, pCreateInfo->image, logicalDevice->depthFormats[i]);
        Logger::debug("tracked depth image view created: appView=" + convertToString(*view)
                      + " sampledView=" + convertToString(sampledView)
                      + " image=" + convertToString(pCreateInfo->image)
                      + " aspect=" + convertToString(pCreateInfo->subresourceRange.aspectMask));
        DepthState depth;
        depth.image = pCreateInfo->image;
        depth.imageView = sampledView;
        depth.format = logicalDevice->depthFormats[i];
        auto extentIt = logicalDevice->depthImageExtents.find(depth.image);
        if (extentIt != logicalDevice->depthImageExtents.end())
            depth.extent = extentIt->second;
        auto metadataIt = logicalDevice->depthImageMetadata.find(depth.image);
        if (metadataIt != logicalDevice->depthImageMetadata.end())
        {
            depth.samples = metadataIt->second.samples;
            depth.transient = (metadataIt->second.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0;
        }
        logicalDevice->depthViewStates[*view] = depth;

        return result;
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_DestroyImageView(VkDevice device, VkImageView imageView, const VkAllocationCallbacks* pAllocator)
    {
        if (!imageView)
            return;

        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            LogicalDevice* logicalDevice = deviceMap[GetKey(device)].get();
            if (logicalDevice == nullptr)
                return;
            logicalDevice->vkd.DestroyImageView(device, imageView, pAllocator);
            return;
        }

        scoped_lock l(globalLock);

        LogicalDevice* logicalDevice = deviceMap[GetKey(device)].get();
        if (logicalDevice == nullptr)
            return;
        logicalDevice->snapshotTargetViewStates.erase(imageView);
        logicalDevice->depthViewStates.erase(imageView);
        clearTrackedDepthScopesLocked(logicalDevice, [imageView](const DepthState& state) { return state.imageView == imageView; });

        for (auto it = logicalDevice->framebufferDepthStates.begin(); it != logicalDevice->framebufferDepthStates.end();)
        {
            if (it->second.imageView == imageView)
                it = logicalDevice->framebufferDepthStates.erase(it);
            else
                ++it;
        }

        if (logicalDevice->activeDepthState.imageView == imageView)
        {
            logicalDevice->activeDepthState = {};
            // Clear stale pin: the pinned view was just destroyed, so
            // getDepthState() would fall back to the (now-empty) active state.
            logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
            DepthState depth = getDepthState(logicalDevice);
            updateDeviceDepthStateLocked(logicalDevice, depth, "DestroyImageView");
        }
        else if (logicalDevice->pinnedDepthImageView == imageView)
        {
            // Pinned view destroyed but it wasn't the active depth — just
            // clear the pin so we fall back to auto-promotion.
            Logger::debug("DestroyImageView: clearing stale pinned depth view");
            logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
        }

        logicalDevice->vkd.DestroyImageView(device, imageView, pAllocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VKIntox_CreateFramebuffer(VkDevice device,
                                                             const VkFramebufferCreateInfo* pCreateInfo,
                                                             const VkAllocationCallbacks* pAllocator,
                                                             VkFramebuffer* pFramebuffer)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            LogicalDevice* logicalDevice = deviceMap[GetKey(device)].get();
            if (logicalDevice == nullptr)
                return VK_ERROR_DEVICE_LOST;
            return logicalDevice->vkd.CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
        }

        scoped_lock l(globalLock);

        LogicalDevice* logicalDevice = deviceMap[GetKey(device)].get();
        if (logicalDevice == nullptr)
            return VK_ERROR_DEVICE_LOST;
        VkResult result = logicalDevice->vkd.CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
        if (result != VK_SUCCESS)
            return result;

        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++)
        {
            auto it = logicalDevice->depthViewStates.find(pCreateInfo->pAttachments[i]);
            if (it != logicalDevice->depthViewStates.end())
            {
                Logger::debug("tracked depth framebuffer attachment: framebuffer=" + convertToString(*pFramebuffer)
                              + " attachmentView=" + convertToString(pCreateInfo->pAttachments[i])
                              + " sampledView=" + convertToString(it->second.imageView)
                              + " image=" + convertToString(it->second.image));
                logicalDevice->framebufferDepthStates[*pFramebuffer] = it->second;
                break;
            }
        }

        DepthSnapshotTarget snapshotTarget = selectDepthSnapshotTargetFromImageViews(
            logicalDevice, pCreateInfo->pAttachments, pCreateInfo->attachmentCount);
        if (snapshotTarget.swapchain != VK_NULL_HANDLE)
        {
            Logger::debug("tracked framebuffer snapshot target: framebuffer=" + convertToString(*pFramebuffer)
                          + " swapchain=" + convertToString(snapshotTarget.swapchain)
                          + " imageIndex=" + std::to_string(snapshotTarget.imageIndex));
            logicalDevice->framebufferSnapshotTargets[*pFramebuffer] = snapshotTarget;
        }
        else
        {
            Logger::debug("framebuffer has no snapshot target: framebuffer=" + convertToString(*pFramebuffer)
                          + " attachments=" + std::to_string(pCreateInfo->attachmentCount));
        }

        return result;
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_DestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks* pAllocator)
    {
        if (!framebuffer)
            return;

        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            LogicalDevice* logicalDevice = deviceMap[GetKey(device)].get();
            if (logicalDevice == nullptr)
                return;
            logicalDevice->vkd.DestroyFramebuffer(device, framebuffer, pAllocator);
            return;
        }

        scoped_lock l(globalLock);

        LogicalDevice* logicalDevice = deviceMap[GetKey(device)].get();
        if (logicalDevice == nullptr)
            return;
        logicalDevice->framebufferDepthStates.erase(framebuffer);
        logicalDevice->framebufferSnapshotTargets.erase(framebuffer);
        logicalDevice->vkd.DestroyFramebuffer(device, framebuffer, pAllocator);
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                                                          const VkRenderPassBeginInfo* pRenderPassBegin,
                                                          VkSubpassContents contents)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                logicalDevice = devIt->second.get();
                VkImageLayout rpDepthFinalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                auto rpLayoutIt = renderPassDepthFinalLayouts.find(pRenderPassBegin->renderPass);
                if (rpLayoutIt != renderPassDepthFinalLayouts.end())
                    rpDepthFinalLayout = rpLayoutIt->second;
                beginTrackedDepthScope(logicalDevice,
                                       commandBuffer,
                                       selectDepthStateFromRenderPassBegin(logicalDevice, pRenderPassBegin),
                                       selectDepthSnapshotTargetFromRenderPassBegin(logicalDevice, pRenderPassBegin),
                                       rpDepthFinalLayout);
            }
        }

        if (logicalDevice)
            logicalDevice->vkd.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdBeginRenderPass2(VkCommandBuffer commandBuffer,
                                                           const VkRenderPassBeginInfo* pRenderPassBegin,
                                                           const VkSubpassBeginInfo* pSubpassBeginInfo)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                logicalDevice = devIt->second.get();
                VkImageLayout rpDepthFinalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                auto rpLayoutIt = renderPassDepthFinalLayouts.find(pRenderPassBegin->renderPass);
                if (rpLayoutIt != renderPassDepthFinalLayouts.end())
                    rpDepthFinalLayout = rpLayoutIt->second;
                beginTrackedDepthScope(logicalDevice,
                                       commandBuffer,
                                       selectDepthStateFromRenderPassBegin(logicalDevice, pRenderPassBegin),
                                       selectDepthSnapshotTargetFromRenderPassBegin(logicalDevice, pRenderPassBegin),
                                       rpDepthFinalLayout);
            }
        }

        if (logicalDevice)
            logicalDevice->vkd.CmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdBeginRenderPass2KHR(VkCommandBuffer commandBuffer,
                                                              const VkRenderPassBeginInfo* pRenderPassBegin,
                                                              const VkSubpassBeginInfo* pSubpassBeginInfo)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdBeginRenderPass2KHR(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                logicalDevice = devIt->second.get();
                VkImageLayout rpDepthFinalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                auto rpLayoutIt = renderPassDepthFinalLayouts.find(pRenderPassBegin->renderPass);
                if (rpLayoutIt != renderPassDepthFinalLayouts.end())
                    rpDepthFinalLayout = rpLayoutIt->second;
                beginTrackedDepthScope(logicalDevice,
                                       commandBuffer,
                                       selectDepthStateFromRenderPassBegin(logicalDevice, pRenderPassBegin),
                                       selectDepthSnapshotTargetFromRenderPassBegin(logicalDevice, pRenderPassBegin),
                                       rpDepthFinalLayout);
            }
        }

        if (logicalDevice)
            logicalDevice->vkd.CmdBeginRenderPass2KHR(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
    }

    void seedTrackedDepthScopeFromRenderingInfo(LogicalDevice* logicalDevice,
                                                VkCommandBuffer commandBuffer,
                                                const VkRenderingInfo* pRenderingInfo)
    {
        VkImageLayout depthFinalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if (pRenderingInfo && pRenderingInfo->pDepthAttachment)
            depthFinalLayout = pRenderingInfo->pDepthAttachment->imageLayout;

        beginTrackedDepthScope(logicalDevice,
                               commandBuffer,
                               selectDepthStateFromRenderingInfo(logicalDevice, pRenderingInfo),
                               selectDepthSnapshotTargetFromRenderingInfo(logicalDevice, pRenderingInfo),
                               depthFinalLayout);
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdBeginRendering(commandBuffer, pRenderingInfo);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                logicalDevice = devIt->second.get();
                seedTrackedDepthScopeFromRenderingInfo(logicalDevice, commandBuffer, pRenderingInfo);
            }
        }

        if (logicalDevice)
        {
            if (pRenderingInfo)
            {
                VkRenderingInfo renderingInfo = *pRenderingInfo;
                VkRenderingAttachmentInfo depthAttachment = {};
                VkRenderingAttachmentInfo stencilAttachment = {};
                bool changed = false;

                if (pRenderingInfo->pDepthAttachment)
                {
                    depthAttachment = *pRenderingInfo->pDepthAttachment;
                    changed = forceDepthAttachmentStoreOp(depthAttachment, false) || changed;
                    renderingInfo.pDepthAttachment = &depthAttachment;
                }

                if (pRenderingInfo->pStencilAttachment)
                {
                    stencilAttachment = *pRenderingInfo->pStencilAttachment;
                    changed = forceDepthAttachmentStoreOp(stencilAttachment, true) || changed;
                    renderingInfo.pStencilAttachment = &stencilAttachment;
                }

                if (changed)
                {
                    Logger::debug("forcing depth attachment storeOp=STORE for VkRenderingInfo on commandBuffer="
                                  + convertToString(commandBuffer));
                    logicalDevice->vkd.CmdBeginRendering(commandBuffer, &renderingInfo);
                    return;
                }
            }

            logicalDevice->vkd.CmdBeginRendering(commandBuffer, pRenderingInfo);
        }
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdBeginRenderingKHR(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdBeginRenderingKHR(commandBuffer, pRenderingInfo);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                logicalDevice = devIt->second.get();
                seedTrackedDepthScopeFromRenderingInfo(logicalDevice, commandBuffer, pRenderingInfo);
            }
        }

        if (logicalDevice)
        {
            if (pRenderingInfo)
            {
                VkRenderingInfo renderingInfo = *pRenderingInfo;
                VkRenderingAttachmentInfo depthAttachment = {};
                VkRenderingAttachmentInfo stencilAttachment = {};
                bool changed = false;

                if (pRenderingInfo->pDepthAttachment)
                {
                    depthAttachment = *pRenderingInfo->pDepthAttachment;
                    changed = forceDepthAttachmentStoreOp(depthAttachment, false) || changed;
                    renderingInfo.pDepthAttachment = &depthAttachment;
                }

                if (pRenderingInfo->pStencilAttachment)
                {
                    stencilAttachment = *pRenderingInfo->pStencilAttachment;
                    changed = forceDepthAttachmentStoreOp(stencilAttachment, true) || changed;
                    renderingInfo.pStencilAttachment = &stencilAttachment;
                }

                if (changed)
                {
                    Logger::debug("forcing depth attachment storeOp=STORE for VkRenderingInfoKHR on commandBuffer="
                                  + convertToString(commandBuffer));
                    logicalDevice->vkd.CmdBeginRenderingKHR(commandBuffer, &renderingInfo);
                    return;
                }
            }

            logicalDevice->vkd.CmdBeginRenderingKHR(commandBuffer, pRenderingInfo);
        }
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdEndRenderPass(VkCommandBuffer commandBuffer)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdEndRenderPass(commandBuffer);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                logicalDevice = devIt->second.get();
        }

        if (logicalDevice)
        {
            logicalDevice->vkd.CmdEndRenderPass(commandBuffer);

            // v3: evaluate candidate and set pending copy. NO command recording.
            const int method = settingsManager.getDepthCaptureMethod();
            if (method == 1 || method == 2)
            {
                scoped_lock l(globalLock);
                DepthState promoted = {};
                DepthSnapshotTarget snapTarget = {};
                VkImageLayout depthFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;

                endTrackedDepthScope(logicalDevice, commandBuffer, "CmdEndRenderPass",
                                     &promoted, &snapTarget, &depthFinalLayout);
                DepthState currentActive = logicalDevice->activeDepthState;

                if (hasDepthState(promoted) && (snapTarget.swapchain != VK_NULL_HANDLE || sameDepthState(currentActive, promoted)))
                {
                    VkImageLayout sourceLayout = (depthFinalLayout != VK_IMAGE_LAYOUT_UNDEFINED)
                        ? depthFinalLayout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    logicalDevice->depthCopy.publish(promoted, sourceLayout);
                }
            }
            else
            {
                // Method 0: just end the scope normally (no capture)
                scoped_lock l(globalLock);
                DepthState promoted = {};
                DepthSnapshotTarget snapTarget = {};
                endTrackedDepthScope(logicalDevice, commandBuffer, "CmdEndRenderPass", &promoted, &snapTarget);
            }
        }
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdEndRenderPass2(VkCommandBuffer commandBuffer, const VkSubpassEndInfo* pSubpassEndInfo)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdEndRenderPass2(commandBuffer, pSubpassEndInfo);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                logicalDevice = devIt->second.get();
        }

        if (logicalDevice)
        {
            logicalDevice->vkd.CmdEndRenderPass2(commandBuffer, pSubpassEndInfo);

            const int method = settingsManager.getDepthCaptureMethod();
            if (method == 1 || method == 2)
            {
                scoped_lock l(globalLock);
                DepthState promoted = {};
                DepthSnapshotTarget snapTarget = {};
                VkImageLayout depthFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;

                endTrackedDepthScope(logicalDevice, commandBuffer, "CmdEndRenderPass2",
                                     &promoted, &snapTarget, &depthFinalLayout);
                DepthState currentActive = logicalDevice->activeDepthState;

                if (hasDepthState(promoted) && (snapTarget.swapchain != VK_NULL_HANDLE || sameDepthState(currentActive, promoted)))
                {
                    VkImageLayout sourceLayout = (depthFinalLayout != VK_IMAGE_LAYOUT_UNDEFINED)
                        ? depthFinalLayout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    logicalDevice->depthCopy.publish(promoted, sourceLayout);
                }
            }
            else
            {
                scoped_lock l(globalLock);
                DepthState promoted = {};
                DepthSnapshotTarget snapTarget = {};
                endTrackedDepthScope(logicalDevice, commandBuffer, "CmdEndRenderPass2", &promoted, &snapTarget);
            }
        }
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdEndRenderPass2KHR(VkCommandBuffer commandBuffer, const VkSubpassEndInfo* pSubpassEndInfo)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdEndRenderPass2KHR(commandBuffer, pSubpassEndInfo);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                logicalDevice = devIt->second.get();
        }

        if (logicalDevice)
        {
            logicalDevice->vkd.CmdEndRenderPass2KHR(commandBuffer, pSubpassEndInfo);

            const int method = settingsManager.getDepthCaptureMethod();
            if (method == 1 || method == 2)
            {
                scoped_lock l(globalLock);
                DepthState promoted = {};
                DepthSnapshotTarget snapTarget = {};
                VkImageLayout depthFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;

                endTrackedDepthScope(logicalDevice, commandBuffer, "CmdEndRenderPass2KHR",
                                     &promoted, &snapTarget, &depthFinalLayout);
                DepthState currentActive = logicalDevice->activeDepthState;

                if (hasDepthState(promoted) && (snapTarget.swapchain != VK_NULL_HANDLE || sameDepthState(currentActive, promoted)))
                {
                    VkImageLayout sourceLayout = (depthFinalLayout != VK_IMAGE_LAYOUT_UNDEFINED)
                        ? depthFinalLayout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    logicalDevice->depthCopy.publish(promoted, sourceLayout);
                }
            }
            else
            {
                scoped_lock l(globalLock);
                DepthState promoted = {};
                DepthSnapshotTarget snapTarget = {};
                endTrackedDepthScope(logicalDevice, commandBuffer, "CmdEndRenderPass2KHR", &promoted, &snapTarget);
            }
        }
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdEndRendering(VkCommandBuffer commandBuffer)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdEndRendering(commandBuffer);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                logicalDevice = devIt->second.get();
        }

        if (logicalDevice)
        {
            logicalDevice->vkd.CmdEndRendering(commandBuffer);

            const int method = settingsManager.getDepthCaptureMethod();
            if (method == 1 || method == 2)
            {
                scoped_lock l(globalLock);
                DepthState promoted = {};
                DepthSnapshotTarget snapTarget = {};
                VkImageLayout depthFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;

                endTrackedDepthScope(logicalDevice, commandBuffer, "CmdEndRendering",
                                     &promoted, &snapTarget, &depthFinalLayout);
                DepthState currentActive = logicalDevice->activeDepthState;

                if (hasDepthState(promoted) && (snapTarget.swapchain != VK_NULL_HANDLE || sameDepthState(currentActive, promoted)))
                {
                    VkImageLayout sourceLayout = (depthFinalLayout != VK_IMAGE_LAYOUT_UNDEFINED)
                        ? depthFinalLayout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    logicalDevice->depthCopy.publish(promoted, sourceLayout);
                }
            }
            else
            {
                scoped_lock l(globalLock);
                DepthState promoted = {};
                DepthSnapshotTarget snapTarget = {};
                endTrackedDepthScope(logicalDevice, commandBuffer, "CmdEndRendering", &promoted, &snapTarget);
            }
        }
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdEndRenderingKHR(VkCommandBuffer commandBuffer)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdEndRenderingKHR(commandBuffer);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                logicalDevice = devIt->second.get();
        }

        if (logicalDevice)
        {
            logicalDevice->vkd.CmdEndRenderingKHR(commandBuffer);

            const int method = settingsManager.getDepthCaptureMethod();
            if (method == 1 || method == 2)
            {
                scoped_lock l(globalLock);
                DepthState promoted = {};
                DepthSnapshotTarget snapTarget = {};
                VkImageLayout depthFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;

                endTrackedDepthScope(logicalDevice, commandBuffer, "CmdEndRenderingKHR",
                                     &promoted, &snapTarget, &depthFinalLayout);
                DepthState currentActive = logicalDevice->activeDepthState;

                if (hasDepthState(promoted) && (snapTarget.swapchain != VK_NULL_HANDLE || sameDepthState(currentActive, promoted)))
                {
                    VkImageLayout sourceLayout = (depthFinalLayout != VK_IMAGE_LAYOUT_UNDEFINED)
                        ? depthFinalLayout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    logicalDevice->depthCopy.publish(promoted, sourceLayout);
                }
            }
            else
            {
                scoped_lock l(globalLock);
                DepthState promoted = {};
                DepthSnapshotTarget snapTarget = {};
                endTrackedDepthScope(logicalDevice, commandBuffer, "CmdEndRenderingKHR", &promoted, &snapTarget);
            }
        }
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdCopyImage(VkCommandBuffer commandBuffer,
                                                    VkImage srcImage,
                                                    VkImageLayout srcImageLayout,
                                                    VkImage dstImage,
                                                    VkImageLayout dstImageLayout,
                                                    uint32_t regionCount,
                                                    const VkImageCopy* pRegions)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdCopyImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                logicalDevice = devIt->second.get();
        }

        if (logicalDevice)
        {
            logicalDevice->vkd.CmdCopyImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
            scoped_lock l(globalLock);
            tryActivatePendingTransferLinkedDepthScope(logicalDevice, commandBuffer, dstImage, "CmdCopyImage");
        }
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdExecuteCommands(VkCommandBuffer commandBuffer,
                                                          uint32_t commandBufferCount,
                                                          const VkCommandBuffer* pCommandBuffers)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                logicalDevice = devIt->second.get();
                accumulateExecutedCommandBufferDraws(logicalDevice, commandBuffer, pCommandBuffers, commandBufferCount);
            }
        }

        if (logicalDevice)
            logicalDevice->vkd.CmdExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers);
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdBlitImage(VkCommandBuffer commandBuffer,
                                                    VkImage srcImage,
                                                    VkImageLayout srcImageLayout,
                                                    VkImage dstImage,
                                                    VkImageLayout dstImageLayout,
                                                    uint32_t regionCount,
                                                    const VkImageBlit* pRegions,
                                                    VkFilter filter)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdBlitImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions, filter);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                logicalDevice = devIt->second.get();
        }

        if (logicalDevice)
        {
            logicalDevice->vkd.CmdBlitImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions, filter);
            scoped_lock l(globalLock);
            tryActivatePendingTransferLinkedDepthScope(logicalDevice, commandBuffer, dstImage, "CmdBlitImage");
        }
    }

    // ReShade-style depth preservation: when the app clears the depth attachment
    // mid-render-pass (vkCmdClearAttachments), record a depth snapshot BEFORE
    // the clear is issued. This captures the pre-clear depth so effects can use
    // it even if the app clears depth at the end of the frame (which Roblox and
    // many deferred renderers do). Without this, the end-of-render-pass snapshot
    // would capture the cleared (empty) depth.
    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdClearAttachments(VkCommandBuffer commandBuffer,
                                                           uint32_t attachmentCount,
                                                           const VkClearAttachment* pAttachments,
                                                           uint32_t rectCount,
                                                           const VkClearRect* pRects)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdClearAttachments(commandBuffer, attachmentCount, pAttachments, rectCount, pRects);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        DepthState depthToPreserve = {};
        DepthSnapshotTarget snapshotTarget = {};
        bool hasDepthClear = false;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                logicalDevice = devIt->second.get();

                // Check if this clear includes the depth attachment we're tracking.
                auto scopeIt = logicalDevice->commandBufferDepthStates.find(commandBuffer);
                if (scopeIt != logicalDevice->commandBufferDepthStates.end()
                    && scopeIt->second.inRenderScope
                    && hasDepthState(scopeIt->second.depthState))
                {
                    for (uint32_t i = 0; i < attachmentCount; i++)
                    {
                        if (pAttachments[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)
                        {
                            depthToPreserve = scopeIt->second.depthState;
                            snapshotTarget = scopeIt->second.snapshotTarget;
                            hasDepthClear = true;
                            break;
                        }
                    }
                }
            }
        }

        // Record a snapshot BEFORE the clear captures the pre-clear depth.
        // This is the key ReShade technique for preserving depth in deferred
        // renderers that clear depth between passes.
        if (logicalDevice && hasDepthClear && hasDepthState(depthToPreserve))
        {
            if (hasPresentableSnapshotTarget(snapshotTarget))
                recordDepthResolveSnapshotForCommandBuffer(logicalDevice, commandBuffer, depthToPreserve, &snapshotTarget);
            else
                recordDepthResolveSnapshotForAllSwapchains(logicalDevice, commandBuffer, depthToPreserve);
        }

        if (logicalDevice)
            logicalDevice->vkd.CmdClearAttachments(commandBuffer, attachmentCount, pAttachments, rectCount, pRects);
    }

    // ReShade-style depth preservation for out-of-render-pass depth clears.
    // If the app calls vkCmdClearDepthStencilImage on a tracked depth image,
    // record a snapshot BEFORE the clear.
    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdClearDepthStencilImage(VkCommandBuffer commandBuffer,
                                                                 VkImage image,
                                                                 VkImageLayout imageLayout,
                                                                 const VkClearDepthStencilValue* pDepthStencil,
                                                                 uint32_t rangeCount,
                                                                 const VkImageSubresourceRange* pRanges)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdClearDepthStencilImage(commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        DepthState depthToPreserve = {};
        bool shouldPreserve = false;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                logicalDevice = devIt->second.get();

                // If this image is the currently-active depth source, preserve it.
                if (logicalDevice->activeDepthState.image == image && hasDepthState(logicalDevice->activeDepthState))
                {
                    depthToPreserve = logicalDevice->activeDepthState;
                    shouldPreserve = true;
                }
            }
        }

        if (logicalDevice && shouldPreserve)
        {
            Logger::debug("CmdClearDepthStencilImage: preserving depth before clear of active depth image="
                          + convertToString(image));
            recordDepthResolveSnapshotForAllSwapchains(logicalDevice, commandBuffer, depthToPreserve);
        }

        if (logicalDevice)
            logicalDevice->vkd.CmdClearDepthStencilImage(commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges);
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdDraw(VkCommandBuffer commandBuffer,
                                               uint32_t vertexCount,
                                               uint32_t instanceCount,
                                               uint32_t firstVertex,
                                               uint32_t firstInstance)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                logicalDevice = devIt->second.get();
                countTrackedDepthDraw(logicalDevice, commandBuffer);
            }
        }

        if (logicalDevice)
            logicalDevice->vkd.CmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdDrawIndexed(VkCommandBuffer commandBuffer,
                                                      uint32_t indexCount,
                                                      uint32_t instanceCount,
                                                      uint32_t firstIndex,
                                                      int32_t vertexOffset,
                                                      uint32_t firstInstance)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                logicalDevice = devIt->second.get();
                countTrackedDepthDraw(logicalDevice, commandBuffer);
            }
        }

        if (logicalDevice)
            logicalDevice->vkd.CmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdDrawIndirect(VkCommandBuffer commandBuffer,
                                                       VkBuffer buffer,
                                                       VkDeviceSize offset,
                                                       uint32_t drawCount,
                                                       uint32_t stride)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndirect(commandBuffer, buffer, offset, drawCount, stride);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                logicalDevice = devIt->second.get();
                countTrackedDepthDraw(logicalDevice, commandBuffer, drawCount);
            }
        }

        if (logicalDevice)
            logicalDevice->vkd.CmdDrawIndirect(commandBuffer, buffer, offset, drawCount, stride);
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdDrawIndexedIndirect(VkCommandBuffer commandBuffer,
                                                              VkBuffer buffer,
                                                              VkDeviceSize offset,
                                                              uint32_t drawCount,
                                                              uint32_t stride)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndexedIndirect(commandBuffer, buffer, offset, drawCount, stride);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                logicalDevice = devIt->second.get();
                countTrackedDepthDraw(logicalDevice, commandBuffer, drawCount);
            }
        }

        if (logicalDevice)
            logicalDevice->vkd.CmdDrawIndexedIndirect(commandBuffer, buffer, offset, drawCount, stride);
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdDrawIndirectCount(VkCommandBuffer commandBuffer,
                                                            VkBuffer buffer,
                                                            VkDeviceSize offset,
                                                            VkBuffer countBuffer,
                                                            VkDeviceSize countBufferOffset,
                                                            uint32_t maxDrawCount,
                                                            uint32_t stride)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                logicalDevice = devIt->second.get();
                countTrackedDepthDraw(logicalDevice, commandBuffer, maxDrawCount);
            }
        }

        if (logicalDevice)
            logicalDevice->vkd.CmdDrawIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdDrawIndirectCountKHR(VkCommandBuffer commandBuffer,
                                                               VkBuffer buffer,
                                                               VkDeviceSize offset,
                                                               VkBuffer countBuffer,
                                                               VkDeviceSize countBufferOffset,
                                                               uint32_t maxDrawCount,
                                                               uint32_t stride)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndirectCountKHR(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                logicalDevice = devIt->second.get();
                countTrackedDepthDraw(logicalDevice, commandBuffer, maxDrawCount);
            }
        }

        if (logicalDevice)
            logicalDevice->vkd.CmdDrawIndirectCountKHR(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdDrawIndexedIndirectCount(VkCommandBuffer commandBuffer,
                                                                   VkBuffer buffer,
                                                                   VkDeviceSize offset,
                                                                   VkBuffer countBuffer,
                                                                   VkDeviceSize countBufferOffset,
                                                                   uint32_t maxDrawCount,
                                                                   uint32_t stride)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndexedIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                logicalDevice = devIt->second.get();
                countTrackedDepthDraw(logicalDevice, commandBuffer, maxDrawCount);
            }
        }

        if (logicalDevice)
            logicalDevice->vkd.CmdDrawIndexedIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_CmdDrawIndexedIndirectCountKHR(VkCommandBuffer commandBuffer,
                                                                      VkBuffer buffer,
                                                                      VkDeviceSize offset,
                                                                      VkBuffer countBuffer,
                                                                      VkDeviceSize countBufferOffset,
                                                                      uint32_t maxDrawCount,
                                                                      uint32_t stride)
    {
        if (!VKIntox::settingsManager.getDepthCapture())
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
                devIt->second->vkd.CmdDrawIndexedIndirectCountKHR(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
            return;
        }

        LogicalDevice* logicalDevice = nullptr;
        {
            scoped_lock l(globalLock);
            auto devIt = deviceMap.find(GetKey(commandBuffer));
            if (devIt != deviceMap.end())
            {
                logicalDevice = devIt->second.get();
                countTrackedDepthDraw(logicalDevice, commandBuffer, maxDrawCount);
            }
        }

        if (logicalDevice)
            logicalDevice->vkd.CmdDrawIndexedIndirectCountKHR(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VKIntox_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                                                              const VkCommandBufferBeginInfo* pBeginInfo)
    {
        scoped_lock l(globalLock);

        auto devIt = deviceMap.find(GetKey(commandBuffer));
        if (devIt == deviceMap.end() || !devIt->second)
        {
            // Not our device — pass through.  Returning an error here would break
            // applications that use multiple VkDevices (only one goes through us).
            // Fall through to the loader's dispatch table.
            // Since we don't have the real dispatch table, the loader will retry.
            // The safest correct action is to return NOT_READY so the loader
            // falls back to the next layer.  However, since we intercepted this
            // call, the dispatch table is already set by the loader to point to
            // our function — so we cannot pass through here.  Return success
            // and let the actual BeginCommandBuffer happen through the normal
            // dispatch path (the command buffer's first pointer routes to the
            // real driver's implementation, not back to us).
            //
            // In practice, this code path is extremely rare because
            // VKIntox_GetDeviceProcAddr only returns our interceptors for
            // devices we created (via the deviceMap lookup at line 4744-4748).
            return VK_SUCCESS;
        }

        LogicalDevice* logicalDevice = devIt->second.get();
        logicalDevice->commandBufferRecordedDrawCounts[commandBuffer] = 0;
        logicalDevice->commandBufferDepthStates.erase(commandBuffer);
        logicalDevice->pendingTransferLinkedDepthScopes.erase(commandBuffer);

        return logicalDevice->vkd.BeginCommandBuffer(commandBuffer, pBeginInfo);
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_FreeCommandBuffers(VkDevice device,
                                                          VkCommandPool commandPool,
                                                          uint32_t commandBufferCount,
                                                          const VkCommandBuffer* pCommandBuffers)
    {
        scoped_lock l(globalLock);

        auto devIt = deviceMap.find(GetKey(device));
        if (devIt == deviceMap.end() || !devIt->second)
            return;  // Not our device — nothing to clean up

        LogicalDevice* logicalDevice = devIt->second.get();
        if (pCommandBuffers)
        {
            for (uint32_t i = 0; i < commandBufferCount; ++i)
            {
                logicalDevice->commandBufferRecordedDrawCounts.erase(pCommandBuffers[i]);
                logicalDevice->commandBufferDepthStates.erase(pCommandBuffers[i]);
                logicalDevice->pendingTransferLinkedDepthScopes.erase(pCommandBuffers[i]);
            }
        }

        logicalDevice->vkd.FreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
    }

    VKAPI_ATTR void VKAPI_CALL VKIntox_DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator)
    {
        if (!image)
            return;

        scoped_lock l(globalLock);

        LogicalDevice* logicalDevice = deviceMap[GetKey(device)].get();
        if (logicalDevice == nullptr)
            return;

        // Check if this is a tracked depth image
        auto it = std::find(logicalDevice->depthImages.begin(), logicalDevice->depthImages.end(), image);
        if (it != logicalDevice->depthImages.end())
        {
            size_t i = std::distance(logicalDevice->depthImages.begin(), it);

            // Remove from tracking lists
            logicalDevice->depthImageExtents.erase(image);
            logicalDevice->depthImageMetadata.erase(image);
            logicalDevice->depthImages.erase(it);
            // TODO what if an image gets destroyed before binding memory?
            if (i < logicalDevice->depthImageViews.size())
            {
                logicalDevice->vkd.DestroyImageView(logicalDevice->device, logicalDevice->depthImageViews[i], nullptr);
                logicalDevice->depthImageViews.erase(logicalDevice->depthImageViews.begin() + i);
            }
            if (i < logicalDevice->depthFormats.size())
                logicalDevice->depthFormats.erase(logicalDevice->depthFormats.begin() + i);

            for (auto viewIt = logicalDevice->depthViewStates.begin(); viewIt != logicalDevice->depthViewStates.end();)
            {
                if (viewIt->second.image == image)
                    viewIt = logicalDevice->depthViewStates.erase(viewIt);
                else
                    ++viewIt;
            }
            for (auto fbIt = logicalDevice->framebufferDepthStates.begin(); fbIt != logicalDevice->framebufferDepthStates.end();)
            {
                if (fbIt->second.image == image)
                    fbIt = logicalDevice->framebufferDepthStates.erase(fbIt);
                else
                    ++fbIt;
            }

            clearTrackedDepthScopesLocked(logicalDevice, [image](const DepthState& state) { return state.image == image; });

            if (logicalDevice->activeDepthState.image == image)
            {
                logicalDevice->activeDepthState = {};
                // Also clear the pin if it references a view of this image
                if (logicalDevice->pinnedDepthImageView != VK_NULL_HANDLE)
                {
                    auto pinIt = logicalDevice->depthViewStates.find(logicalDevice->pinnedDepthImageView);
                    if (pinIt == logicalDevice->depthViewStates.end() || pinIt->second.image == image)
                        logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
                }
                DepthState depth = getDepthState(logicalDevice);
                updateDeviceDepthStateLocked(logicalDevice, depth, "DestroyImage");
            }
            else
            {
                // Image wasn't active, but clear pin if it referenced this image
                if (logicalDevice->pinnedDepthImageView != VK_NULL_HANDLE)
                {
                    auto pinIt = logicalDevice->depthViewStates.find(logicalDevice->pinnedDepthImageView);
                    if (pinIt != logicalDevice->depthViewStates.end() && pinIt->second.image == image)
                    {
                        Logger::debug("DestroyImage: clearing stale pinned depth view (image destroyed)");
                        logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
                    }
                }
            }
        }

        logicalDevice->vkd.DestroyImage(logicalDevice->device, image, pAllocator);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Wayland surface interception — capture wl_display for input

    VKAPI_ATTR VkResult VKAPI_CALL VKIntox_CreateWaylandSurfaceKHR(
        VkInstance                              instance,
        const VkWaylandSurfaceCreateInfoKHR*    pCreateInfo,
        const VkAllocationCallbacks*            pAllocator,
        VkSurfaceKHR*                           pSurface)
    {
        scoped_lock l(globalLock);

        Logger::trace("vkCreateWaylandSurfaceKHR");

        // Capture the wl_display and wl_surface for Wayland input
        if (pCreateInfo && pCreateInfo->display)
            setWaylandDisplay(pCreateInfo->display);
        if (pCreateInfo && pCreateInfo->surface)
            setWaylandSurface(pCreateInfo->surface);

        // Forward to the real implementation via the next layer
        auto nextFunc = (PFN_vkCreateWaylandSurfaceKHR)
            instanceDispatchMap[GetKey(instance)].GetInstanceProcAddr(
                instanceMap[GetKey(instance)], "vkCreateWaylandSurfaceKHR");
        if (!nextFunc)
            return VK_ERROR_EXTENSION_NOT_PRESENT;

        return nextFunc(instance, pCreateInfo, pAllocator, pSurface);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VKIntox_CreateXlibSurfaceKHR(
        VkInstance                           instance,
        const VkXlibSurfaceCreateInfoKHR*    pCreateInfo,
        const VkAllocationCallbacks*         pAllocator,
        VkSurfaceKHR*                        pSurface)
    {
        scoped_lock l(globalLock);

        Logger::trace("vkCreateXlibSurfaceKHR");
        if (pCreateInfo && pCreateInfo->window)
            setX11Window((unsigned long)pCreateInfo->window);

        auto nextFunc = (PFN_vkCreateXlibSurfaceKHR)
            instanceDispatchMap[GetKey(instance)].GetInstanceProcAddr(
                instanceMap[GetKey(instance)], "vkCreateXlibSurfaceKHR");
        if (!nextFunc)
            return VK_ERROR_EXTENSION_NOT_PRESENT;

        return nextFunc(instance, pCreateInfo, pAllocator, pSurface);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VKIntox_CreateXcbSurfaceKHR(
        VkInstance                          instance,
        const VkXcbSurfaceCreateInfoKHR*    pCreateInfo,
        const VkAllocationCallbacks*        pAllocator,
        VkSurfaceKHR*                       pSurface)
    {
        scoped_lock l(globalLock);

        Logger::trace("vkCreateXcbSurfaceKHR");
        if (pCreateInfo && pCreateInfo->window)
            setX11Window((unsigned long)pCreateInfo->window);

        auto nextFunc = (PFN_vkCreateXcbSurfaceKHR)
            instanceDispatchMap[GetKey(instance)].GetInstanceProcAddr(
                instanceMap[GetKey(instance)], "vkCreateXcbSurfaceKHR");
        if (!nextFunc)
            return VK_ERROR_EXTENSION_NOT_PRESENT;

        return nextFunc(instance, pCreateInfo, pAllocator, pSurface);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Enumeration function

    VkResult VKAPI_CALL VKIntox_EnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties)
    {
        if (pPropertyCount)
            *pPropertyCount = 1;

        if (pProperties)
        {
            std::strcpy(pProperties->layerName, VKINTOX_LAYER_NAME);
            std::strcpy(pProperties->description, "a post processing layer");
            pProperties->implementationVersion = 1;
            pProperties->specVersion           = VK_MAKE_VERSION(1, 2, 0);
        }

        return VK_SUCCESS;
    }

    VkResult VKAPI_CALL VKIntox_EnumerateDeviceLayerProperties(VkPhysicalDevice   /* physicalDevice */,
                                                                uint32_t*          pPropertyCount,
                                                                VkLayerProperties* pProperties)
    {
        return VKIntox_EnumerateInstanceLayerProperties(pPropertyCount, pProperties);
    }

    VkResult VKAPI_CALL VKIntox_EnumerateInstanceExtensionProperties(const char*            pLayerName,
                                                                      uint32_t*              pPropertyCount,
                                                                      VkExtensionProperties* /* pProperties */)
    {
        if (pLayerName == NULL || std::strcmp(pLayerName, VKINTOX_LAYER_NAME))
        {
            return VK_ERROR_LAYER_NOT_PRESENT;
        }

        // don't expose any extensions
        if (pPropertyCount)
        {
            *pPropertyCount = 0;
        }
        return VK_SUCCESS;
    }

    VkResult VKAPI_CALL VKIntox_EnumerateDeviceExtensionProperties(VkPhysicalDevice       physicalDevice,
                                                                    const char*            pLayerName,
                                                                    uint32_t*              pPropertyCount,
                                                                    VkExtensionProperties* pProperties)
    {
        // pass through any queries that aren't to us
        if (pLayerName == NULL || std::strcmp(pLayerName, VKINTOX_LAYER_NAME))
        {
            if (physicalDevice == VK_NULL_HANDLE)
            {
                return VK_SUCCESS;
            }

            scoped_lock l(globalLock);
            auto it = instanceDispatchMap.find(GetKey(physicalDevice));
            if (it == instanceDispatchMap.end() || !it->second.EnumerateDeviceExtensionProperties)
                return VK_ERROR_INITIALIZATION_FAILED;
            return it->second.EnumerateDeviceExtensionProperties(
                physicalDevice, pLayerName, pPropertyCount, pProperties);
        }

        // don't expose any extensions
        if (pPropertyCount)
        {
            *pPropertyCount = 0;
        }
        return VK_SUCCESS;
    }
} // namespace VKIntox

extern "C"
{ // these are the entry points for the layer, so they need to be c-linkeable

    VK_SHADE_EXPORT PFN_vkVoidFunction VKAPI_CALL VKIntox_GetDeviceProcAddr(VkDevice device, const char* pName);
    VK_SHADE_EXPORT PFN_vkVoidFunction VKAPI_CALL VKIntox_GetInstanceProcAddr(VkInstance instance, const char* pName);

    static PFN_vkGetInstanceProcAddr getNextInstanceProcAddr()
    {
        return reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(RTLD_NEXT, "vkGetInstanceProcAddr"));
    }

    static PFN_vkGetDeviceProcAddr getNextDeviceProcAddr()
    {
        return reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(RTLD_NEXT, "vkGetDeviceProcAddr"));
    }

#define GETPROCADDR(func) \
    if (!std::strcmp(pName, "vk" #func)) \
        return (PFN_vkVoidFunction) &VKIntox::VKIntox_##func;
    /*
    Return our funktions for the funktions we want to intercept
    the macro takes the name and returns our VKIntox_##func, if the name is equal
    */

    // vkGetDeviceProcAddr needs to behave like vkGetInstanceProcAddr thanks to some games
#define INTERCEPT_CALLS \
    /* instance chain functions we intercept */ \
    if (!std::strcmp(pName, "vkGetInstanceProcAddr")) \
        return (PFN_vkVoidFunction) &VKIntox_GetInstanceProcAddr; \
    GETPROCADDR(EnumerateInstanceLayerProperties); \
    GETPROCADDR(EnumerateInstanceExtensionProperties); \
    GETPROCADDR(CreateInstance); \
    GETPROCADDR(DestroyInstance); \
    GETPROCADDR(CreateWaylandSurfaceKHR); \
    GETPROCADDR(CreateXlibSurfaceKHR); \
    GETPROCADDR(CreateXcbSurfaceKHR); \
\
    /* device chain functions we intercept*/ \
    if (!std::strcmp(pName, "vkGetDeviceProcAddr")) \
        return (PFN_vkVoidFunction) &VKIntox_GetDeviceProcAddr; \
    GETPROCADDR(EnumerateDeviceLayerProperties); \
    GETPROCADDR(EnumerateDeviceExtensionProperties); \
    GETPROCADDR(CreateDevice); \
    GETPROCADDR(DestroyDevice); \
    GETPROCADDR(CreateSwapchainKHR); \
    GETPROCADDR(GetSwapchainImagesKHR); \
    GETPROCADDR(QueuePresentKHR); \
    GETPROCADDR(QueueSubmit); \
    GETPROCADDR(DestroySwapchainKHR); \
\
    GETPROCADDR(CreateImage); \
    GETPROCADDR(DestroyImage); \
    GETPROCADDR(BindImageMemory); \
    GETPROCADDR(CreateImageView); \
    GETPROCADDR(DestroyImageView); \
    GETPROCADDR(CreateRenderPass); \
    GETPROCADDR(CreateRenderPass2); \
    GETPROCADDR(CreateRenderPass2KHR); \
    GETPROCADDR(DestroyRenderPass); \
    GETPROCADDR(CreateFramebuffer); \
    GETPROCADDR(DestroyFramebuffer); \
    GETPROCADDR(BeginCommandBuffer); \
    GETPROCADDR(FreeCommandBuffers); \
    GETPROCADDR(CmdBeginRenderPass); \
    GETPROCADDR(CmdBeginRenderPass2); \
    GETPROCADDR(CmdBeginRenderPass2KHR); \
    GETPROCADDR(CmdBeginRendering); \
    GETPROCADDR(CmdBeginRenderingKHR); \
    GETPROCADDR(CmdEndRenderPass); \
    GETPROCADDR(CmdEndRenderPass2); \
    GETPROCADDR(CmdEndRenderPass2KHR); \
    GETPROCADDR(CmdEndRendering); \
    GETPROCADDR(CmdEndRenderingKHR); \
    GETPROCADDR(CmdBlitImage); \
    GETPROCADDR(CmdClearAttachments); \
    GETPROCADDR(CmdClearDepthStencilImage); \
    GETPROCADDR(CmdCopyImage); \
    GETPROCADDR(CmdExecuteCommands); \
    GETPROCADDR(CmdDraw); \
    GETPROCADDR(CmdDrawIndexed); \
    GETPROCADDR(CmdDrawIndirect); \
    GETPROCADDR(CmdDrawIndexedIndirect); \
    GETPROCADDR(CmdDrawIndirectCount); \
    GETPROCADDR(CmdDrawIndirectCountKHR); \
    GETPROCADDR(CmdDrawIndexedIndirectCount); \
    GETPROCADDR(CmdDrawIndexedIndirectCountKHR); \

    VK_SHADE_EXPORT PFN_vkVoidFunction VKAPI_CALL VKIntox_GetDeviceProcAddr(VkDevice device, const char* pName)
    {
        VKIntox::initConfigs();

        INTERCEPT_CALLS

        if (device == VK_NULL_HANDLE)
        {
            PFN_vkGetDeviceProcAddr next = getNextDeviceProcAddr();
            return next ? next(device, pName) : nullptr;
        }

        {
            VKIntox::scoped_lock l(VKIntox::globalLock);
            auto it = VKIntox::deviceMap.find(VKIntox::GetKey(device));
            if (it != VKIntox::deviceMap.end() && it->second && it->second->vkd.GetDeviceProcAddr)
                return it->second->vkd.GetDeviceProcAddr(device, pName);
        }

        PFN_vkGetDeviceProcAddr next = getNextDeviceProcAddr();
        return next ? next(device, pName) : nullptr;
    }

    VK_SHADE_EXPORT PFN_vkVoidFunction VKAPI_CALL VKIntox_GetInstanceProcAddr(VkInstance instance, const char* pName)
    {
        VKIntox::initConfigs();

        INTERCEPT_CALLS

        if (instance == VK_NULL_HANDLE)
        {
            PFN_vkGetInstanceProcAddr next = getNextInstanceProcAddr();
            return next ? next(instance, pName) : nullptr;
        }

        {
            VKIntox::scoped_lock l(VKIntox::globalLock);
            auto it = VKIntox::instanceDispatchMap.find(VKIntox::GetKey(instance));
            if (it != VKIntox::instanceDispatchMap.end() && it->second.GetInstanceProcAddr)
                return it->second.GetInstanceProcAddr(instance, pName);
        }

        PFN_vkGetInstanceProcAddr next = getNextInstanceProcAddr();
        return next ? next(instance, pName) : nullptr;
    }

} // extern "C"
