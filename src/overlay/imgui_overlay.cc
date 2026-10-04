#include "imgui_overlay.hh"

#include "vk_handle.hh"
#include "effects/effect_registry.hh"
#include "settings_manager.hh"
#include "reshade_parser.hh"
#include "logger.hh"
#include "util.hh"
#include "mouse_input.hh"
#include "keyboard_input.hh"
#include "input_blocker.hh"
#include "config_serializer.hh"
#include "async_writer.hh"
#include "image.hh"
#include "memory.hh"
#include "overlay/vkintox_icon_png.hh"
#include "stb_image.h"
#include "wayland_display.hh"
#include "wayland_pointer_constraints.hh"
#include "wayland_input_common.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <memory>

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imgui_internal.h"
#include "vendor/imgui/imgui_m3.h"
#include "overlay/ui_theme.hh"
#include "overlay/ui_icons.hh"
#include "vendor/imgui/backends/imgui_impl_vulkan.h"

namespace VKIntox
{
    namespace
    {
        // compact like a macOS titlebar: just enough for the brand row.
        float OverlayTitleBarHeight()
        {
            return 44.0f * ImGuiM3GetMetrics().density;
        }

        void overlayTitleHeightConstraint(ImGuiSizeCallbackData* data)
        {
            data->DesiredSize.y = std::max(data->DesiredSize.y, OverlayTitleBarHeight() + 1.0f);
        }

        // Cubic-bezier timing function, the same shape Hyprland uses for its
        // window animations: P0=(0,0), P1=(x1,y1), P2=(x2,y2), P3=(1,1). x is the
        // normalised clock, y is the eased value; y may exceed 1 (x2=1.12 here),
        // which is the deliberate overshoot. Solved by bisection, so it can never
        // diverge the way an integrated spring can when frames are uneven.
        float CubicBezierEase(float x, float x1, float y1, float x2, float y2)
        {
            x = ImClamp(x, 0.0f, 1.0f);
            auto bezier = [](float t, float p1, float p2) {
                const float u = 1.0f - t;
                return 3.0f * p1 * t * u * u + 3.0f * p2 * t * t * u + t * t * t;
            };

            float lo = 0.0f, hi = 1.0f;
            for (int i = 0; i < 24; ++i)
            {
                const float mid = (lo + hi) * 0.5f;
                if (bezier(mid, x1, x2) < x)
                    lo = mid;
                else
                    hi = mid;
            }
            return bezier((lo + hi) * 0.5f, y1, y2);
        }
    }

    // No-op dummy for Vulkan functions ImGui requests but VKIntox doesn't intercept.
    // ImGui's LoadFunctions treats nullptr returns as failures, so we need a valid pointer.
    static void VKAPI_CALL dummyVulkanFunc() {}

    // Function loader using VKIntox's dispatch tables
    static PFN_vkVoidFunction imguiVulkanLoaderDummy(const char* function_name, void* user_data)
    {
        LogicalDevice* device = static_cast<LogicalDevice*>(user_data);

        // Device functions from VKIntox's dispatch table
        #define CHECK_FUNC(name) if (strcmp(function_name, "vk" #name) == 0) return (PFN_vkVoidFunction)device->vkd.name

        CHECK_FUNC(AllocateCommandBuffers);
        CHECK_FUNC(AllocateDescriptorSets);
        CHECK_FUNC(AllocateMemory);
        CHECK_FUNC(BeginCommandBuffer);
        CHECK_FUNC(BindBufferMemory);
        CHECK_FUNC(BindImageMemory);
        CHECK_FUNC(CmdBeginRenderPass);
        CHECK_FUNC(CmdBindDescriptorSets);
        CHECK_FUNC(CmdBindIndexBuffer);
        CHECK_FUNC(CmdBindPipeline);
        CHECK_FUNC(CmdBindVertexBuffers);
        CHECK_FUNC(CmdCopyBufferToImage);
        CHECK_FUNC(CmdDrawIndexed);
        CHECK_FUNC(CmdEndRenderPass);
        CHECK_FUNC(CmdPipelineBarrier);
        CHECK_FUNC(CmdPushConstants);
        CHECK_FUNC(CmdSetScissor);
        CHECK_FUNC(CmdSetViewport);
        CHECK_FUNC(CreateBuffer);
        CHECK_FUNC(CreateCommandPool);
        CHECK_FUNC(CreateDescriptorPool);
        CHECK_FUNC(CreateDescriptorSetLayout);
        CHECK_FUNC(CreateFence);
        CHECK_FUNC(CreateFramebuffer);
        CHECK_FUNC(CreateGraphicsPipelines);
        CHECK_FUNC(CreateImage);
        CHECK_FUNC(CreateImageView);
        CHECK_FUNC(CreatePipelineLayout);
        CHECK_FUNC(CreateRenderPass);
        CHECK_FUNC(CreateSampler);
        CHECK_FUNC(CreateSemaphore);
        CHECK_FUNC(CreateShaderModule);
        CHECK_FUNC(CreateSwapchainKHR);
        CHECK_FUNC(DestroyBuffer);
        CHECK_FUNC(DestroyCommandPool);
        CHECK_FUNC(DestroyDescriptorPool);
        CHECK_FUNC(DestroyDescriptorSetLayout);
        CHECK_FUNC(DestroyFence);
        CHECK_FUNC(DestroyFramebuffer);
        CHECK_FUNC(DestroyImage);
        CHECK_FUNC(DestroyImageView);
        CHECK_FUNC(DestroyPipeline);
        CHECK_FUNC(DestroyPipelineLayout);
        CHECK_FUNC(DestroyRenderPass);
        CHECK_FUNC(DestroySampler);
        CHECK_FUNC(DestroySemaphore);
        CHECK_FUNC(DestroyShaderModule);
        CHECK_FUNC(DestroySwapchainKHR);
        CHECK_FUNC(EndCommandBuffer);
        CHECK_FUNC(FlushMappedMemoryRanges);
        CHECK_FUNC(FreeCommandBuffers);
        CHECK_FUNC(FreeDescriptorSets);
        CHECK_FUNC(FreeMemory);
        CHECK_FUNC(GetBufferMemoryRequirements);
        CHECK_FUNC(GetDeviceQueue);
        CHECK_FUNC(GetImageMemoryRequirements);
        CHECK_FUNC(GetSwapchainImagesKHR);
        CHECK_FUNC(MapMemory);
        CHECK_FUNC(QueueSubmit);
        CHECK_FUNC(QueueWaitIdle);
        CHECK_FUNC(ResetCommandPool);
        CHECK_FUNC(ResetFences);
        CHECK_FUNC(UnmapMemory);
        CHECK_FUNC(UpdateDescriptorSets);
        CHECK_FUNC(WaitForFences);
        // Not in the layer's dispatch table; resolve it directly.
        if (strcmp(function_name, "vkGetFenceStatus") == 0)
            return (PFN_vkVoidFunction)device->vkd.GetDeviceProcAddr(device->device, "vkGetFenceStatus");
        #undef CHECK_FUNC

        // Instance functions from VKIntox's dispatch
        #define CHECK_IFUNC(name) if (strcmp(function_name, "vk" #name) == 0) return (PFN_vkVoidFunction)device->vki.name
        CHECK_IFUNC(GetPhysicalDeviceMemoryProperties);
        CHECK_IFUNC(GetPhysicalDeviceProperties);
        CHECK_IFUNC(GetPhysicalDeviceQueueFamilyProperties);
        #undef CHECK_IFUNC

        // Return a no-op dummy for unknown functions. ImGui's LoadFunctions treats
        // nullptr as a load failure, so we must return a valid function pointer.
        // These functions are never actually called by ImGui in our usage.
        return (PFN_vkVoidFunction)dummyVulkanFunc;
    }

    ImGuiOverlay::ImGuiOverlay(LogicalDevice* device, VkFormat swapchainFormat, uint32_t imageCount, OverlayPersistentState* persistentState)
        : logicalDevice(device), pPersistentState(persistentState)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        // imconfig disables ImGui's default shell handler, so TextLinkOpenURL
        // would silently do nothing. route it through our xdg-open helper.
        ImGui::GetPlatformIO().Platform_OpenInShellFn = [](ImGuiContext*, const char* url) -> bool
        {
            return openInShell(url);
        };
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigWindowsMoveFromTitleBarOnly = true;

        std::string iniPath = ConfigSerializer::getBaseConfigDir() + "/imgui.ini";
        std::ifstream iniFile(iniPath);
        std::string iniContent((std::istreambuf_iterator<char>(iniFile)),
                                std::istreambuf_iterator<char>());

        if (!iniContent.empty())
            ImGui::LoadIniSettingsFromDisk(iniPath.c_str());

        // ── Material 3 Expressive theme ────────────────────────────────────
        // The whole skin lives in vendor/imgui/imgui_m3.{h,cc}: the token set,
        // the `.colors` file, live reload, and the M3 widget painting. Here we
        // only point it at a file and let it populate the style.
        ImGuiM3SetThemeFile(ImGuiM3DefaultThemeFilePath());
        if (const char* themePath = ImGuiM3GetThemeFile())
            Logger::info("ImGui: Material 3 Expressive theme from " + std::string(themePath));

        // flatpak sandboxes each have their own config dir, and setup only seeds the
        // app it ran for. so look in ours, then in every sandbox's.
        std::string baseConfigDir = ConfigSerializer::getBaseConfigDir();
        std::vector<std::string> fontDirs = {baseConfigDir + "/font"};
        if (const char* home = std::getenv("HOME"))
        {
            const std::string varApp = std::string(home) + "/.var/app";
            std::error_code ec;
            if (std::filesystem::is_directory(varApp, ec))
            {
                for (const auto& entry : std::filesystem::directory_iterator(varApp, ec))
                {
                    if (!entry.is_directory())
                        continue;
                    std::string dir = entry.path().string() + "/config/VKIntox/font";
                    if (dir != fontDirs.front())
                        fontDirs.push_back(std::move(dir));
                }
            }
        }

        auto findFont = [&](const std::string& filename) -> std::string
        {
            for (const auto& dir : fontDirs)
            {
                std::string path = dir + "/" + filename;
                if (std::ifstream(path).good())
                    return path;
            }
            return "";
        };

        std::vector<std::pair<std::string, std::string>> fontSearchPaths = {
            {findFont("GoogleSans-Regular.ttf"), "Google Sans Regular"},
            {findFont("font.ttf"), "legacy regular fallback"}
        };

        // AddFontFromFileTTF copies the file in, so a borrowed pointer is fine.
        const char* regularPath = nullptr;
        for (const auto& [path, desc] : fontSearchPaths)
        {
            if (std::ifstream(path).good()) { regularPath = path.c_str(); break; }
        }

        // M3 body-medium: 14sp, regular weight. everything else steps from here.
        constexpr float kBodyMediumSize = 14.0f;

        ImFontConfig fontCfg;
        fontCfg.SizePixels = kBodyMediumSize;
        fontCfg.OversampleH = 2;
        fontCfg.OversampleV = 1;
        fontCfg.PixelSnapH = true;

        // Material Symbols: merged into every text face for inline codepoints,
        // plus a standalone 24px face for M3Icon and the nav.
        const std::string iconFontPath = findFont("MaterialSymbolsRounded.ttf");
        const bool haveIconFont = !iconFontPath.empty();

        if (regularPath)
        {
            io.Fonts->Clear();

            // merge icons in before the next face so it starts fresh.
            auto addFace = [&](const char* path) -> ImFont*
            {
                ImFont* face = io.Fonts->AddFontFromFileTTF(path, kBodyMediumSize, &fontCfg);
                if (face && haveIconFont)
                    ImGuiM3MergeIconFont(iconFontPath.c_str(), kBodyMediumSize, Icon::GlyphRanges());
                return face;
            };

            ImFont* regular = addFace(regularPath);
            ImFont* medium = nullptr;
            ImFont* bold = nullptr;
            ImFont* extraBold = nullptr;
            // medium for display/headline emphasis, bold for titles and labels.
            const std::pair<const char*, ImFont**> weights[] = {
                {"GoogleSans-Medium.ttf", &medium},
                {"GoogleSans-Bold.ttf", &bold},
                {"GoogleSans-ExtraBold.ttf", &extraBold},
            };
            for (const auto& [filename, slot] : weights)
            {
                const std::string path = findFont(filename);
                if (!path.empty())
                    *slot = addFace(path.c_str());
            }
            // merging appends atlas entries, so pass the faces by identity.
            ImGuiM3SetTextFonts(regular, medium, bold, extraBold);
            Logger::info("ImGui: loaded Google Sans Flex regular/medium/bold weights from " + std::string(regularPath));
        }
        else
        {
            Logger::warn("ImGui: Google Sans Flex not found - using default font");
        }

        if (haveIconFont)
        {
            if (ImGuiM3LoadIconFont(iconFontPath.c_str(), 24.0f, Icon::GlyphRanges()))
                Logger::info("ImGui: loaded Material Symbols icon font");
            else
                Logger::warn("ImGui: Material Symbols font could not be parsed");
        }
        else
        {
            Logger::warn("ImGui: Material Symbols font not found - icons disabled");
        }

        // frame padding needs the real font metrics, so re-apply now.
        ImGuiM3ApplyToStyle(1.0f);

        initVulkanBackend(swapchainFormat, imageCount);

        // Restore UI preferences from persistent state
        if (pPersistentState)
            visible = pPersistentState->visible;
        setInputBlocked(visible);

        initialized = true;
        Logger::info("ImGui overlay initialized");
    }


    ImGuiOverlay::~ImGuiOverlay()
    {
        if (!initialized) return;

        // Auto-save profile on shutdown (before GPU cleanup)
        if ((profileDirty || paramsDirty) && (!activeProfilePath.empty() || !activeShaderProfilePath.empty()))
            autoSaveProfile();

        // the auto-save is queued on the writer thread; make sure it lands before
        // the layer tears down rather than racing process exit.
        AsyncWriter::instance().waitForIdle();

        logicalDevice->vkd.QueueWaitIdle(logicalDevice->queue);

        // Clean up Wayland resources before destroying the event queue
        if (isWayland())
            cleanupPointerConstraints();

        std::string iniPath = ConfigSerializer::getBaseConfigDir() + "/imgui.ini";
        ImGui::SaveIniSettingsToDisk(iniPath.c_str());

        if (titleIconDescriptor != VK_NULL_HANDLE)
            ImGui_ImplVulkan_RemoveTexture(titleIconDescriptor);

        // The ImGui teardown stays inline and runs first, because it releases
        // ImGui's own references to the descriptor pool and the title icon
        // descriptor. Those references have to be gone before the queue releases
        // the pool, and the queue only runs at the next teardown, so ordering
        // here is by construction rather than by luck.
        if (backendInitialized)
            ImGui_ImplVulkan_Shutdown();
        ImGui::DestroyContext();

        // Deferred, like every other owner. The QueueWaitIdle above already makes
        // this a safe moment, but inline destruction still meant that an overlay
        // torn down during device teardown would call into a destroyed VkDevice.
        // Handing the handles over means the flush at device destroy decides when
        // they actually go, and the handle is still valid when it does.
        auto& queue   = DeferredDestroyQueue::instance();
        auto  device = logicalDevice->device;
        auto& vkd    = logicalDevice->vkd;

        const VkDeviceMemory   iconMem = titleIconMemory;
        const VkImage         iconImg = titleIconImage;
        const VkImageView     iconView = titleIconView;
        const VkSampler       iconSampler = titleIconSampler;
        const VkCommandPool   pool = commandPool;
        const VkRenderPass    pass = renderPass;
        const VkDescriptorPool descPool = descriptorPool;

        if (iconMem != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Memory, [vkd, device, iconMem] { vkd.FreeMemory(device, iconMem, nullptr); });
        // image before its view, so the view is released first
        if (iconImg != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Resource, [vkd, device, iconImg] { vkd.DestroyImage(device, iconImg, nullptr); });
        if (iconView != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Resource, [vkd, device, iconView] { vkd.DestroyImageView(device, iconView, nullptr); });
        if (iconSampler != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Resource, [vkd, device, iconSampler] { vkd.DestroySampler(device, iconSampler, nullptr); });
        if (pool != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Resource, [vkd, device, pool] { vkd.DestroyCommandPool(device, pool, nullptr); });

        if (descPool != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Descriptor, [vkd, device, descPool] { vkd.DestroyDescriptorPool(device, descPool, nullptr); });

        // framebuffers before the pass they were created from
        for (auto fb : framebuffers)
        {
            if (fb != VK_NULL_HANDLE)
                queue.push(DestroyPhase::RenderPass, [vkd, device, fb] { vkd.DestroyFramebuffer(device, fb, nullptr); });
        }
        if (pass != VK_NULL_HANDLE)
            queue.push(DestroyPhase::RenderPass, [vkd, device, pass] { vkd.DestroyRenderPass(device, pass, nullptr); });

        // Fences are the earliest phase: a fence whose command pool is gone is
        // fine to destroy, but a command pool destroyed while a submitted fence
        // is still unsignalled is not, and the pool lands in Resource.
        for (auto fence : commandBufferFences)
        {
            if (fence != VK_NULL_HANDLE)
                queue.push(DestroyPhase::Sync, [vkd, device, fence] { vkd.DestroyFence(device, fence, nullptr); });
        }

        Logger::info("ImGui overlay destroyed");
    }

    void ImGuiOverlay::toggle()
    {
        visible = !visible;
        setInputBlocked(visible);

        // Pointer confinement was removed: the overlay renders inside the game's
        // surface, so confining the pointer doesn't prevent the compositor from
        // interpreting clicks as window-move grabs.

        saveToPersistentState();
    }

    void ImGuiOverlay::refreshShaderProfiles()
    {
        shaderProfiles = ConfigSerializer::listShaderProfilesForGame(activeGameName);
        if (activeShaderProfileName.empty())
        {
            const auto lastUsedProfile = ConfigSerializer::getLastShaderProfile(activeGameName);
            if (std::find(shaderProfiles.begin(), shaderProfiles.end(), lastUsedProfile) != shaderProfiles.end())
                activeShaderProfileName = lastUsedProfile;
        }
        if (activeShaderProfileName.empty() ||
            std::find(shaderProfiles.begin(), shaderProfiles.end(), activeShaderProfileName) == shaderProfiles.end())
        {
            const auto defaultProfile = std::find(shaderProfiles.begin(), shaderProfiles.end(), "default");
            activeShaderProfileName = defaultProfile != shaderProfiles.end()
                ? *defaultProfile
                : (shaderProfiles.empty() ? std::string() : shaderProfiles.front());
        }
        activeShaderProfilePath = ConfigSerializer::getShaderProfilePath(activeGameName, activeShaderProfileName);
        if (!activeGameName.empty() && !activeShaderProfileName.empty())
            ConfigSerializer::setLastShaderProfile(activeGameName, activeShaderProfileName);
    }

    void ImGuiOverlay::setActiveShaderProfile(const std::string& profileName)
    {
        activeShaderProfileName = profileName;
        activeShaderProfilePath = ConfigSerializer::getShaderProfilePath(activeGameName, activeShaderProfileName);
        if (!activeGameName.empty() && !activeShaderProfileName.empty())
            ConfigSerializer::setLastShaderProfile(activeGameName, activeShaderProfileName);
    }

    bool ImGuiOverlay::switchShaderProfile(const std::string& profileName)
    {
        if (profileName.empty() || profileName == activeShaderProfileName)
            return true;

        // save the outgoing profile while it is still active, then clear the
        // dirty flags so render()'s auto-save can't clobber the new path.
        if (!autoSaveProfile(true))
            return false;

        setActiveShaderProfile(profileName);
        pendingShaderProfilePath = activeShaderProfilePath;
        pendingShaderProfile = true;
        applyRequested = true;
        paramsDirty = false;
        profileDirty = false;
        return true;
    }

    void ImGuiOverlay::renderCenteredBrandIcon(float size)
    {
        if (titleIconDescriptor == VK_NULL_HANDLE || size <= 0.0f)
            return;
        size = std::min(size, 512.0f);
        const float contentWidth = ImGui::GetContentRegionAvail().x;
        const float x = ImGui::GetCursorPosX() + std::max(0.0f, (contentWidth - size) * 0.5f);
        ImGui::SetCursorPosX(x);
        ImGui::GetWindowDrawList()->AddImage(
            ImTextureRef(reinterpret_cast<ImTextureID>(titleIconDescriptor)),
            ImGui::GetCursorScreenPos(),
            ImVec2(ImGui::GetCursorScreenPos().x + size, ImGui::GetCursorScreenPos().y + size));
        ImGui::Dummy(ImVec2(size, size));
        ImGui::Dummy(ImVec2(0.0f, std::max(6.0f, size * 0.035f)));
        const char* brandText = "VKIntox";
        // wordmark is the only display-scale run, so it takes bold.
        ImFont* font = ImGuiM3FontBold();
        if (!font)
            font = ImGui::GetIO().Fonts->Fonts[0];
        ImGui::PushFont(font, std::max(font->LegacySize, size * 0.14f));
        const float textWidth = ImGui::CalcTextSize(brandText).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (contentWidth - textWidth) * 0.5f));
        ImGui::TextUnformatted(brandText);
        ImGui::PopFont();
    }

    void ImGuiOverlay::pushToast(LogLevel level, const std::string& message)
    {
        std::lock_guard<std::mutex> lock(toastsMutex);
        // Avoid stacking identical messages — refresh timestamp instead.
        for (auto& t : toasts)
        {
            if (t.message == message && t.level == level)
            {
                t.createdAt = std::chrono::steady_clock::now();
                return;
            }
        }
        toasts.push_back({message, level, std::chrono::steady_clock::now()});
        // Hard cap so a runaway panic loop can't exhaust memory.
        if (toasts.size() > 8)
            toasts.erase(toasts.begin(), toasts.end() - 8);
    }

    bool ImGuiOverlay::hasPendingToasts() const
    {
        std::lock_guard<std::mutex> lock(toastsMutex);
        return !toasts.empty();
    }

    void ImGuiOverlay::renderToasts()
    {
        // Snapshot under the lock so we can render without holding it.
        std::vector<ToastNotification> snapshot;
        {
            std::lock_guard<std::mutex> lock(toastsMutex);
            snapshot = toasts;
        }
        if (snapshot.empty())
            return;

        constexpr float kLifetimeSeconds = 5.0f;
        constexpr float kFadeSeconds = 0.6f;
        const auto now = std::chrono::steady_clock::now();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float margin = 28.0f;
        const float gap = 10.0f;
        const float padX = 22.0f;
        const float padY = 16.0f;
        const float shadowPad = 14.0f;
        const float rounding = 16.0f;
        // never let a collapsed game window produce a negative card width
        const float maxCardWidth = ImMax(160.0f, ImMin(560.0f, viewport->WorkSize.x - margin * 2.0f));

        std::vector<size_t> expired;
        for (size_t i = 0; i < snapshot.size(); ++i)
            if (std::chrono::duration<float>(now - snapshot[i].createdAt).count() >= kLifetimeSeconds)
                expired.push_back(i);

        // the newest toast sits nearest the bottom edge, older ones stack upward
        float cursorY = viewport->WorkPos.y + viewport->WorkSize.y - margin;
        for (size_t i = 0; i < snapshot.size(); ++i)
        {
            const float age = std::chrono::duration<float>(now - snapshot[i].createdAt).count();
            if (age >= kLifetimeSeconds)
                continue;
            // ramp out over the last fraction of a second instead of popping
            const float fade = age > kLifetimeSeconds - kFadeSeconds
                ? std::clamp((kLifetimeSeconds - age) / kFadeSeconds, 0.0f, 1.0f)
                : 1.0f;

            const std::string& message = snapshot[i].message;
            const float wrapWidth = maxCardWidth - padX * 2.0f;
            const ImVec2 textSize = ImGui::CalcTextSize(message.c_str(), nullptr, false, wrapWidth);
            const float cardWidth = ImMin(maxCardWidth, ImMax(textSize.x, 1.0f) + padX * 2.0f);
            const float cardHeight = textSize.y + padY * 2.0f;
            const float windowWidth = cardWidth + shadowPad * 2.0f;
            const float windowHeight = cardHeight + shadowPad * 2.0f;

            cursorY -= windowHeight;
            const ImVec2 windowPos(viewport->WorkPos.x + (viewport->WorkSize.x - windowWidth) * 0.5f, cursorY);
            ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_Always);

            // NoInputs: the toast is informational, so clicks pass through to the
            // game instead of being withheld by the input blocker.
            const ImGuiWindowFlags flags =
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                ImGuiWindowFlags_NoInputs;

            // transparent host window: the card and its shadow are drawn inside,
            // which keeps the whole thing on the top-most window layer.
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

            char label[32];
            snprintf(label, sizeof(label), "##vkintoxToast%zu", i);
            const bool open = ImGui::Begin(label, nullptr, flags);

            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(2);

            if (open)
            {
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                const ImVec2 cardMin(windowPos.x + shadowPad, windowPos.y + shadowPad);
                const ImVec2 cardMax(cardMin.x + cardWidth, cardMin.y + cardHeight);

                // fake a soft drop shadow: stacked translucent rounded rects,
                // largest first so density builds toward the card edge
                const int shadowAlpha = static_cast<int>(7.0f * fade);
                const int cardAlpha = static_cast<int>(255.0f * fade);
                for (int step = static_cast<int>(shadowPad); step >= 1; --step)
                {
                    drawList->AddRectFilled(ImVec2(cardMin.x - step, cardMin.y - step),
                                            ImVec2(cardMax.x + step, cardMax.y + step),
                                            IM_COL32(0, 0, 0, shadowAlpha), rounding + step);
                }
                drawList->AddRectFilled(cardMin, cardMax, IM_COL32(255, 255, 255, cardAlpha), rounding);

                ImGui::SetCursorScreenPos(ImVec2(cardMin.x + padX, cardMin.y + padY));
                ImGui::PushTextWrapPos(cardMin.x + cardWidth - padX);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, fade));
                ImGui::TextUnformatted(message.c_str());
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
            }
            ImGui::End();

            cursorY -= gap;
        }

        if (!expired.empty())
        {
            std::lock_guard<std::mutex> lock(toastsMutex);
            // Erase in reverse so indices stay valid.
            for (auto it = expired.rbegin(); it != expired.rend(); ++it)
            {
                if (*it < toasts.size())
                    toasts.erase(toasts.begin() + *it);
            }
        }
    }

    void ImGuiOverlay::saveToPersistentState()
    {
        if (!pPersistentState)
            return;

        // Only save UI preferences - effect state is in the registry, settings in settingsManager
        pPersistentState->visible = visible;
    }

    void ImGuiOverlay::updateState(OverlayState newState)
    {
        // only the fields the add-effects view reads; a change here invalidates
        // its cached entry list
        if (newState.currentConfigEffects != state.currentConfigEffects
            || newState.defaultConfigEffects != state.defaultConfigEffects
            || newState.effectPaths != state.effectPaths)
            ++overlayStateVersion;

        state = std::move(newState);

        if (!effectRegistry)
            return;

        // Registry is already initialized from config at swapchain creation
        // Just ensure any newly added effects are in the registry
        const auto& selectedEffects = effectRegistry->getSelectedEffects();
        for (const auto& effectName : selectedEffects)
        {
            if (!effectRegistry->hasEffect(effectName))
                effectRegistry->ensureEffect(effectName);
        }
        // No editableParams merging needed - Registry IS the source of truth
    }

    std::vector<std::unique_ptr<EffectParam>> ImGuiOverlay::getModifiedParams() const
    {
        if (!effectRegistry)
            return {};
        return effectRegistry->getAllParameters();
    }

    std::vector<std::string> ImGuiOverlay::getActiveEffects() const
    {
        std::vector<std::string> activeEffects;
        if (!effectRegistry)
            return activeEffects;

        for (const auto& effectName : effectRegistry->getSelectedEffects())
        {
            if (effectRegistry->isEffectEnabled(effectName))
                activeEffects.push_back(effectName);
        }
        return activeEffects;
    }

    const std::vector<std::string>& ImGuiOverlay::getSelectedEffects() const
    {
        static std::vector<std::string> empty;
        return effectRegistry ? effectRegistry->getSelectedEffects() : empty;
    }

    void ImGuiOverlay::collectSaveData(
        std::vector<std::string>& effects,
        std::vector<std::string>& disabledEffects,
        std::vector<ConfigParam>& params,
        std::map<std::string, std::string>& effectPaths,
        std::vector<PreprocessorDefinition>& allDefs,
        std::vector<ConfigParam>& disabledEffectParams)
    {
        if (!effectRegistry)
            return;

        effects = effectRegistry->getSelectedEffects();

        for (const auto& effectName : effects)
        {
            const bool effectEnabled = effectRegistry->isEffectEnabled(effectName);
            for (auto* p : effectRegistry->getParametersForEffect(effectName))
            {
                if (p->noSave)
                    continue;
                auto serialized = p->serialize();
                for (const auto& [suffix, value] : serialized)
                {
                    ConfigParam cp;
                    cp.effectName = p->effectName;
                    cp.paramName = suffix.empty() ? p->name : suffix;
                    cp.value = value;
                    params.push_back(cp);
                    if (!effectEnabled)
                        disabledEffectParams.push_back(cp);
                }
            }

            if (!effectEnabled)
                disabledEffects.push_back(effectName);

            std::string path = effectRegistry->getEffectFilePath(effectName);
            if (!path.empty())
                effectPaths[effectName] = path;

            const auto& defs = effectRegistry->getPreprocessorDefs(effectName);
            for (const auto& def : defs)
            {
                allDefs.push_back(def);
                if (!effectEnabled)
                    disabledEffectParams.push_back({def.effectName, "@" + def.name, def.value});
            }
        }
    }

    void ImGuiOverlay::saveCurrentConfig()
    {
        if (!effectRegistry)
            return;

        std::vector<std::string> effects, disabledEffects;
        std::vector<ConfigParam> params;
        std::vector<ConfigParam> disabledEffectParams;
        std::map<std::string, std::string> effectPaths;
        std::vector<PreprocessorDefinition> allDefs;
        collectSaveData(effects, disabledEffects, params, effectPaths, allDefs, disabledEffectParams);

        ConfigSerializer::saveConfig(saveConfigName, effects, disabledEffects, params, effectPaths, allDefs);
        configListRefreshPending = true;
        profileDirty = false;
    }

    bool ImGuiOverlay::autoSaveProfile(bool block)
    {
        if (!effectRegistry)
            return false;
        if (activeProfilePath.empty() && activeShaderProfilePath.empty())
            return true;

        std::vector<std::string> effects, disabledEffects;
        std::vector<ConfigParam> params;
        std::vector<ConfigParam> disabledEffectParams;
        std::map<std::string, std::string> effectPaths;
        std::vector<PreprocessorDefinition> allDefs;
        collectSaveData(effects, disabledEffects, params, effectPaths, allDefs, disabledEffectParams);

        // Snapshot the registry-derived data here: the write runs on the writer
        // thread and must not touch state the present thread mutates. EffectConfig
        // owns unique_ptrs and cannot be copied, so everything the job needs is
        // flattened into plain values now.
        const std::string profilePath = activeProfilePath;
        const std::string shaderPath = activeShaderProfilePath;

        std::set<std::string> disabledFiles;
        std::vector<std::string> enabledTechniques;
        std::vector<std::string> techniqueSorting;
        if (!shaderPath.empty())
        {
            const auto& allEffects = effectRegistry->getAllEffects();
            for (const auto& effectName : disabledEffects)
            {
                const auto effect = std::find_if(allEffects.begin(), allEffects.end(), [&effectName](const EffectConfig& item) {
                    return item.name == effectName;
                });
                if (effect != allEffects.end() && !effect->filePath.empty())
                    disabledFiles.insert(std::filesystem::path(effect->filePath).filename().string());
            }
            const auto& selected = effectRegistry->getSelectedEffects();
            for (const auto& name : selected)
            {
                auto effect = std::find_if(allEffects.begin(), allEffects.end(), [&name](const EffectConfig& item) {
                    return item.name == name;
                });
                if (effect == allEffects.end() || effect->filePath.empty())
                    continue;
                const std::string filename = std::filesystem::path(effect->filePath).filename().string();
                for (const auto& technique : effect->techniqueNames)
                {
                    const std::string entry = technique + "@" + filename;
                    techniqueSorting.push_back(entry);
                    if (effectRegistry->isEffectEnabled(name))
                        enabledTechniques.push_back(entry);
                }
            }
        }

        // the job records whether the write succeeded so a blocking caller can
        // still fail the operation; an async caller only sees the log line.
        auto ok = std::make_shared<std::atomic<bool>>(true);

        AsyncWriter::instance().submit(
            [profilePath, shaderPath, effects, disabledEffects, params, effectPaths,
             allDefs, disabledEffectParams, disabledFiles, enabledTechniques, techniqueSorting, ok]() mutable
            {
                bool configSaved = true;
                if (!profilePath.empty())
                {
                    // .conf carries the ReShade definitions only; values live in the
                    // .ini so a sparse preset can't inherit stale ones.
                    std::map<std::string, std::string> instancePaths = effectPaths;
                    for (const auto& [name, path] : effectPaths)
                    {
                        if (std::filesystem::path(path).extension() == ".fx")
                            instancePaths[name] = std::filesystem::path(path).filename().string();
                    }
                    configSaved = ConfigSerializer::saveToPath(profilePath, {}, {}, {},
                                                               instancePaths, {});
                }

                bool shaderSaved = true;
                if (!shaderPath.empty())
                {
                    std::vector<ConfigParam> shaderParams = params;
                    shaderParams.erase(std::remove_if(shaderParams.begin(), shaderParams.end(),
                        [&disabledFiles, &effectPaths](const ConfigParam& param) {
                            const auto path = effectPaths.find(param.effectName);
                            return path != effectPaths.end() &&
                                disabledFiles.count(std::filesystem::path(path->second).filename().string()) != 0;
                        }),
                        shaderParams.end());
                    disabledEffectParams.erase(std::remove_if(disabledEffectParams.begin(), disabledEffectParams.end(),
                        [&disabledFiles, &effectPaths](const ConfigParam& param) {
                            const auto path = effectPaths.find(param.effectName);
                            return path != effectPaths.end() &&
                                disabledFiles.count(std::filesystem::path(path->second).filename().string()) == 0;
                        }),
                        disabledEffectParams.end());
                    for (const auto& def : allDefs)
                    {
                        ConfigParam param{def.effectName, "@" + def.name, def.value};
                        shaderParams.push_back(std::move(param));
                    }
                    std::set<std::pair<std::string, std::string>> enabledParamKeys;
                    for (const auto& param : shaderParams)
                        enabledParamKeys.emplace(param.effectName, param.paramName);
                    disabledEffectParams.erase(std::remove_if(disabledEffectParams.begin(), disabledEffectParams.end(),
                        [&enabledParamKeys](const ConfigParam& param) {
                            return enabledParamKeys.count({param.effectName, param.paramName}) != 0;
                        }), disabledEffectParams.end());
                    shaderSaved = ConfigSerializer::saveShaderProfile(shaderPath, shaderParams, effects, disabledEffects,
                                                                       effectPaths, enabledTechniques, techniqueSorting,
                                                                       disabledEffectParams);
                }

                if (shaderSaved && configSaved)
                    Logger::debug("Auto-saved profile: " + profilePath);
                else
                    Logger::err("Auto-save failed for profile: " + profilePath);
                ok->store(shaderSaved && configSaved, std::memory_order_relaxed);
            });

        // the write is queued; mark clean now so the debounce does not re-arm.
        profileDirty = false;
        if (block)
        {
            AsyncWriter::instance().waitForIdle();
            return ok->load(std::memory_order_relaxed);
        }
        return true;
    }

    void ImGuiOverlay::setSelectedEffects(const std::vector<std::string>& effects,
                                          const std::vector<std::string>& disabledEffects)
    {
        if (!effectRegistry)
            return;

        effectRegistry->setSelectedEffects(effects);

        // Build set of disabled effects for quick lookup
        std::set<std::string> disabledSet(disabledEffects.begin(), disabledEffects.end());

        // Set enabled states in registry: disabled if in disabledEffects, enabled otherwise
        for (const auto& effectName : effects)
        {
            bool enabled = (disabledSet.find(effectName) == disabledSet.end());
            effectRegistry->setEffectEnabled(effectName, enabled);
        }
    }

    void ImGuiOverlay::initVulkanBackend(VkFormat swapchainFormat, uint32_t imageCount)
    {
        // Load Vulkan functions for ImGui using VKIntox's dispatch tables
        bool loaded = ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, imguiVulkanLoaderDummy, logicalDevice);
        if (!loaded)
        {
            Logger::err("Failed to load Vulkan functions for ImGui");
            return;
        }
        Logger::debug("ImGui Vulkan functions loaded");

        // Create descriptor pool for ImGui
        VkDescriptorPoolSize poolSizes[] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 }
        };

        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 100;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = poolSizes;

        VkResult vr = logicalDevice->vkd.CreateDescriptorPool(logicalDevice->device, &poolInfo, nullptr, &descriptorPool);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to create ImGui descriptor pool: " + std::to_string(vr));
            return;
        }

        // Create render pass for ImGui
        VkAttachmentDescription attachment = {};
        attachment.format = swapchainFormat;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef = {};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        // Dependency for incoming: wait for prior color writes before overlay renders.
        // srcAccessMask MUST include COLOR_ATTACHMENT_WRITE (or TRANSFER_WRITE) so
        // that the implicit PRESENT_SRC→COLOR_ATTACHMENT_OPTIMAL layout transition
        // does not race with the effect pass that just wrote to the swapchain image.
        // The semaphore between effect-submit and overlay-submit guarantees
        // execution ordering, but the render-pass dependency governs the implicit
        // transition barrier and must correctly describe the source access.
        VkSubpassDependency dependencies[2] = {};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        // Dependency for outgoing: ensure overlay writes + layout transition complete
        // and are VISIBLE to presentation engine and DMA-BUF screen capture readers.
        // VK_ACCESS_MEMORY_READ_BIT is critical — without it, GPU caches may not be
        // flushed before PipeWire/compositor reads the image via DMA-BUF.
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

        VkRenderPassCreateInfo renderPassInfo = {};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &attachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 2;
        renderPassInfo.pDependencies = dependencies;

        vr = logicalDevice->vkd.CreateRenderPass(logicalDevice->device, &renderPassInfo, nullptr, &renderPass);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to create ImGui render pass: " + std::to_string(vr));
            return;
        }

        // Initialize ImGui Vulkan backend
        ImGui_ImplVulkan_InitInfo initInfo = {};
        initInfo.ApiVersion = VK_API_VERSION_1_3;
        initInfo.Instance = logicalDevice->instance;
        initInfo.PhysicalDevice = logicalDevice->physicalDevice;
        initInfo.Device = logicalDevice->device;
        initInfo.QueueFamily = logicalDevice->queueFamilyIndex;
        initInfo.Queue = logicalDevice->queue;
        initInfo.DescriptorPool = descriptorPool;
        // ImageCount MUST match the actual swapchain image count.
        // Hardcoding 2 while the real swapchain has 3 (triple-buffering)
        // causes ImGui's internal vertex/index ring buffer to wrap too early,
        // overwriting buffers still in-flight on the GPU → ghost trails,
        // stale vertices, and black corruption artefacts.
        initInfo.MinImageCount = imageCount;
        initInfo.ImageCount = imageCount;
        initInfo.PipelineInfoMain.RenderPass = renderPass;

        ImGui_ImplVulkan_Init(&initInfo);

        VkImageCreateInfo iconInfo{};
        iconInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iconInfo.imageType = VK_IMAGE_TYPE_2D;
        iconInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        iconInfo.extent = {512, 512, 1};
        iconInfo.mipLevels = 1;
        iconInfo.arrayLayers = 1;
        iconInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        iconInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        iconInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        iconInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        iconInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult iconResult = logicalDevice->vkd.CreateImage(logicalDevice->device, &iconInfo, nullptr, &titleIconImage);
        if (iconResult == VK_SUCCESS)
        {
            VkMemoryRequirements requirements{};
            logicalDevice->vkd.GetImageMemoryRequirements(logicalDevice->device, titleIconImage, &requirements);
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = findMemoryTypeIndex(logicalDevice, requirements.memoryTypeBits,
                                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            iconResult = logicalDevice->vkd.AllocateMemory(logicalDevice->device, &allocation, nullptr, &titleIconMemory);
            if (iconResult == VK_SUCCESS)
                iconResult = logicalDevice->vkd.BindImageMemory(logicalDevice->device, titleIconImage, titleIconMemory, 0);
        }
        if (iconResult == VK_SUCCESS)
        {
            int iconWidth = 0, iconHeight = 0, iconChannels = 0;
            stbi_uc* iconPixels = stbi_load_from_memory(kOverlayIconPng, sizeof(kOverlayIconPng),
                                                        &iconWidth, &iconHeight, &iconChannels, STBI_rgb_alpha);
            if (!iconPixels || iconWidth != 512 || iconHeight != 512)
            {
                if (iconPixels)
                    stbi_image_free(iconPixels);
                iconResult = VK_ERROR_FORMAT_NOT_SUPPORTED;
                Logger::warn("Could not decode embedded VKIntox icon");
            }
            else
            {
                uploadToImage(logicalDevice, titleIconImage,
                              {static_cast<uint32_t>(iconWidth), static_cast<uint32_t>(iconHeight), 1},
                              static_cast<uint32_t>(iconWidth * iconHeight * 4), iconPixels);
                stbi_image_free(iconPixels);
            }
        }
        if (iconResult == VK_SUCCESS)
        {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = titleIconImage;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            iconResult = logicalDevice->vkd.CreateImageView(logicalDevice->device, &viewInfo, nullptr, &titleIconView);
        }
        if (iconResult == VK_SUCCESS)
        {
            VkSamplerCreateInfo samplerInfo{};
            samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            samplerInfo.magFilter = VK_FILTER_LINEAR;
            samplerInfo.minFilter = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.maxLod = 0.0f;
            iconResult = logicalDevice->vkd.CreateSampler(logicalDevice->device, &samplerInfo, nullptr, &titleIconSampler);
        }
        if (iconResult == VK_SUCCESS)
            titleIconDescriptor = ImGui_ImplVulkan_AddTexture(titleIconSampler, titleIconView,
                                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        else
            Logger::warn("Could not create VKIntox title bar icon: " + std::to_string(iconResult));

        this->swapchainFormat = swapchainFormat;
        this->imageCount = imageCount;
        bool commandBuffersAllocated = false;

        auto rollbackBackendInit = [&]() {
            for (auto fence : commandBufferFences)
            {
                if (fence != VK_NULL_HANDLE)
                    logicalDevice->vkd.DestroyFence(logicalDevice->device, fence, nullptr);
            }
            commandBufferFences.clear();

            if (commandBuffersAllocated && !commandBuffers.empty() && commandPool != VK_NULL_HANDLE)
            {
                logicalDevice->vkd.FreeCommandBuffers(
                    logicalDevice->device, commandPool, static_cast<uint32_t>(commandBuffers.size()), commandBuffers.data());
            }
            commandBuffers.clear();

            for (auto fb : framebuffers)
            {
                if (fb != VK_NULL_HANDLE)
                    logicalDevice->vkd.DestroyFramebuffer(logicalDevice->device, fb, nullptr);
            }
            framebuffers.clear();
            framebufferImageViews.clear();

            if (commandPool != VK_NULL_HANDLE)
            {
                logicalDevice->vkd.DestroyCommandPool(logicalDevice->device, commandPool, nullptr);
                commandPool = VK_NULL_HANDLE;
            }
            if (renderPass != VK_NULL_HANDLE)
            {
                logicalDevice->vkd.DestroyRenderPass(logicalDevice->device, renderPass, nullptr);
                renderPass = VK_NULL_HANDLE;
            }
            if (descriptorPool != VK_NULL_HANDLE)
            {
                logicalDevice->vkd.DestroyDescriptorPool(logicalDevice->device, descriptorPool, nullptr);
                descriptorPool = VK_NULL_HANDLE;
            }

            ImGui_ImplVulkan_Shutdown();
            backendInitialized = false;
        };

        // Create command pool
        VkCommandPoolCreateInfo poolCreateInfo = {};
        poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolCreateInfo.queueFamilyIndex = logicalDevice->queueFamilyIndex;
        vr = logicalDevice->vkd.CreateCommandPool(logicalDevice->device, &poolCreateInfo, nullptr, &commandPool);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to create ImGui command pool: " + std::to_string(vr));
            rollbackBackendInit();
            return;
        }

        // Allocate command buffers
        commandBuffers.resize(imageCount);
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = imageCount;
        vr = logicalDevice->vkd.AllocateCommandBuffers(logicalDevice->device, &allocInfo, commandBuffers.data());
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to allocate ImGui command buffers: " + std::to_string(vr));
            rollbackBackendInit();
            return;
        }
        commandBuffersAllocated = true;

        // Create fences for command buffer synchronization (signaled initially so first frame doesn't wait)
        commandBufferFences.resize(imageCount);
        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < imageCount; i++)
        {
            vr = logicalDevice->vkd.CreateFence(logicalDevice->device, &fenceInfo, nullptr, &commandBufferFences[i]);
            if (vr != VK_SUCCESS)
            {
                Logger::err("Failed to create ImGui fence " + std::to_string(i) + ": " + std::to_string(vr));
                rollbackBackendInit();
                return;
            }
        }

        backendInitialized = true;
        Logger::debug("ImGui Vulkan backend initialized");
    }

    VkCommandBuffer ImGuiOverlay::recordFrame(uint32_t imageIndex, VkImageView imageView, uint32_t width, uint32_t height)
    {
        // Render even when the main overlay is hidden if there are pending
        // toast notifications — fatal errors must stay visible to the user.
        const bool hasToasts = hasPendingToasts();
        if (!backendInitialized || (!visible && !hasToasts))
        {
            // drop last frame's hitboxes, or they keep swallowing input.
            frameInputRects.clear();
            setInputRects(nullptr, 0);
            setInputBlocked(false);
            setWaylandInputSurfaceRect(0.0f, 0.0f, 0.0f, 0.0f);
            return VK_NULL_HANDLE;
        }

        // hitboxes are rebuilt below; the gate is only on while the overlay (or
        // a toast) is actually on screen.
        frameInputRects.clear();
        setInputBlocked((visible || hasToasts) && settingsManager.getOverlayBlockInput());

        // Store current resolution for VRAM estimates in settings
        currentWidth = width;
        currentHeight = height;

        // Wait for previous use of this command buffer to complete.
        // Adaptive timeout: 4x the measured frame time (generous margin for GPU
        // scheduling jitter), clamped to [2ms, 50ms].  With triple buffering the
        // fence was submitted 2-3 frames ago and is almost always already signaled.
        static float avgFrameTimeMs = 8.0f;  // Seed at ~120 FPS
        VkFence fence = commandBufferFences[imageIndex];
        uint64_t timeoutNs = static_cast<uint64_t>(
            std::clamp(avgFrameTimeMs * 4.0f, 2.0f, 50.0f) * 1'000'000.0f);
        VkResult fenceResult = logicalDevice->vkd.WaitForFences(logicalDevice->device, 1, &fence, VK_TRUE, timeoutNs);
        if (fenceResult == VK_TIMEOUT)
        {
            Logger::warn("ImGui fence wait timed out for image " + std::to_string(imageIndex));
            return VK_NULL_HANDLE;
        }
        if (fenceResult != VK_SUCCESS)
        {
            Logger::err("ImGui fence wait failed: " + std::to_string(fenceResult));
            return VK_NULL_HANDLE;
        }
        VkResult resetResult = logicalDevice->vkd.ResetFences(logicalDevice->device, 1, &fence);
        if (resetResult != VK_SUCCESS)
        {
            Logger::err("Failed to reset ImGui fence: " + std::to_string(resetResult));
            return VK_NULL_HANDLE;
        }

        VkCommandBuffer cmd = commandBuffers[imageIndex];

        // Begin command buffer
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkResult vr = logicalDevice->vkd.BeginCommandBuffer(cmd, &beginInfo);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to begin ImGui command buffer: " + std::to_string(vr));
            return VK_NULL_HANDLE;
        }

        // Ensure framebuffer exists for this image index.
        // Recreate if the image view or dimensions changed (swapchain recreation).
        if (framebuffers.size() <= imageIndex)
        {
            framebuffers.resize(imageIndex + 1, VK_NULL_HANDLE);
            framebufferImageViews.resize(imageIndex + 1, VK_NULL_HANDLE);
        }

        bool needRecreate = (framebuffers[imageIndex] == VK_NULL_HANDLE) ||
                            (framebufferImageViews[imageIndex] != imageView) ||
                            (framebufferWidth != width) || (framebufferHeight != height);

        if (needRecreate)
        {
            if (framebuffers[imageIndex] != VK_NULL_HANDLE)
                logicalDevice->vkd.DestroyFramebuffer(logicalDevice->device, framebuffers[imageIndex], nullptr);

            VkFramebufferCreateInfo fbInfo = {};
            fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = renderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments = &imageView;
            fbInfo.width = width;
            fbInfo.height = height;
            fbInfo.layers = 1;
            vr = logicalDevice->vkd.CreateFramebuffer(logicalDevice->device, &fbInfo, nullptr, &framebuffers[imageIndex]);
            if (vr != VK_SUCCESS)
            {
                Logger::err("Failed to create ImGui framebuffer: " + std::to_string(vr));
                logicalDevice->vkd.EndCommandBuffer(cmd);
                return VK_NULL_HANDLE;
            }
            framebufferImageViews[imageIndex] = imageView;
            framebufferWidth = width;
            framebufferHeight = height;
        }

        VkFramebuffer framebuffer = framebuffers[imageIndex];

        // Set display size and frame timing BEFORE NewFrame
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)width, (float)height);

        // DeltaTime for ImGui's internal timing (drag thresholds, animations).
        // Uses last known good value as fallback instead of hardcoded 1/60.
        static auto lastFrameTime = std::chrono::steady_clock::now();
        static float lastGoodDt = 0.008f;  // Seed at ~120 FPS
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;
        if (dt > 0.0f && dt < 1.0f)
        {
            io.DeltaTime = dt;
            lastGoodDt = dt;
            // Update adaptive fence timeout with exponential moving average
            avgFrameTimeMs = avgFrameTimeMs * 0.9f + (dt * 1000.0f) * 0.1f;
        }
        else
        {
            io.DeltaTime = lastGoodDt;
        }

        // Mouse input for interactivity
        MouseState mouse = getMouseState();
        io.MousePos = ImVec2((float)mouse.x, (float)mouse.y);

        io.MouseDown[0] = mouse.leftButton;
        io.MouseDown[1] = mouse.rightButton;
        io.MouseDown[2] = mouse.middleButton;
        io.MouseWheel = mouse.scrollDelta;
        io.MouseDrawCursor = true;

        auto publishOverlayInput = [&]() {
            if (settingsManager.getOverlayBlockInput())
                setInputRects(frameInputRects.data(), static_cast<int>(frameInputRects.size()));
            else
                setInputRects(nullptr, 0);
            updatePointerPosition(static_cast<float>(mouse.x), static_cast<float>(mouse.y));
        };

        // Keyboard input for text fields
        // Keys are one-shot events, so we send press and release in same frame
        KeyboardState keyboard = getKeyboardState();
        for (char c : keyboard.typedChars)
            io.AddInputCharacter(c);
        if (keyboard.backspace) { io.AddKeyEvent(ImGuiKey_Backspace, true); io.AddKeyEvent(ImGuiKey_Backspace, false); }
        if (keyboard.del) { io.AddKeyEvent(ImGuiKey_Delete, true); io.AddKeyEvent(ImGuiKey_Delete, false); }
        if (keyboard.enter) { io.AddKeyEvent(ImGuiKey_Enter, true); io.AddKeyEvent(ImGuiKey_Enter, false); }
        if (keyboard.left) { io.AddKeyEvent(ImGuiKey_LeftArrow, true); io.AddKeyEvent(ImGuiKey_LeftArrow, false); }
        if (keyboard.right) { io.AddKeyEvent(ImGuiKey_RightArrow, true); io.AddKeyEvent(ImGuiKey_RightArrow, false); }
        if (keyboard.home) { io.AddKeyEvent(ImGuiKey_Home, true); io.AddKeyEvent(ImGuiKey_Home, false); }
        if (keyboard.end) { io.AddKeyEvent(ImGuiKey_End, true); io.AddKeyEvent(ImGuiKey_End, false); }

        // ImGui frame
        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();

        // When the overlay is hidden but toasts are showing, skip the main
        // window — toasts are standalone floating windows.
        if (!visible)
        {
            // toasts render on their own so fatal errors stay visible
            renderToasts();
            // main window is gone; only toasts remain, so free the pointer.
            setWaylandInputSurfaceRect(0.0f, 0.0f, 0.0f, 0.0f);
            publishOverlayInput();
            ImGui::Render();

            VkRenderPassBeginInfo rpBegin = {};
            rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpBegin.renderPass = renderPass;
            rpBegin.framebuffer = framebuffer;
            rpBegin.renderArea.extent.width = width;
            rpBegin.renderArea.extent.height = height;

            logicalDevice->vkd.CmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
            logicalDevice->vkd.CmdEndRenderPass(cmd);

            VkResult endRes = logicalDevice->vkd.EndCommandBuffer(cmd);
            if (endRes != VK_SUCCESS)
            {
                Logger::err("Failed to end ImGui command buffer (toast-only): " + std::to_string(endRes));
                return VK_NULL_HANDLE;
            }
            return cmd;
        }

        // the overlay draws into the game's swapchain, so all window math lives in
        // framebuffer space -- monitor work area is the wrong coordinate system
        // for a windowed game.
        const ImVec2 screenMin(0.0f, 0.0f);
        const ImVec2 screenMax(static_cast<float>(width), static_cast<float>(height));
        const ImVec2 avail(screenMax.x - screenMin.x, screenMax.y - screenMin.y);

        // default: tall panel on the right, sized off the game window so it isn't
        // starving on a large display and doesn't overflow a small one.
        const float margin_x = avail.x * 0.04f;
        const float margin_y = avail.y * 0.03f;
        const float panel_w = ImClamp(avail.x * 0.36f, 320.0f, ImMax(320.0f, avail.x - margin_x * 2.0f));
        const float panel_h = ImClamp(avail.y * 0.91f, 360.0f, ImMax(360.0f, avail.y - margin_y * 2.0f));
        const ImVec2 defaultPos(screenMax.x - panel_w - margin_x, screenMin.y + margin_y);
        const ImVec2 defaultSize(panel_w, panel_h);

        const ImVec2 minSize(ImMin(300.0f, avail.x), ImMin(200.0f, avail.y));
        const ImVec2 maxSize = avail;
        if (resetLayoutRequested)
        {
            ImGui::SetNextWindowPos(defaultPos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(defaultSize, ImGuiCond_Always);
            resetLayoutRequested = false;
        }
        else
        {
            ImGui::SetNextWindowPos(defaultPos, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(defaultSize, ImGuiCond_FirstUseEver);
        }

        const float previousFramePaddingY = ImGui::GetStyle().FramePadding.y;
        ImGui::GetStyle().FramePadding.y = std::max(0.0f, (OverlayTitleBarHeight() - ImGui::GetFontSize()) * 0.5f);
        ImGui::SetNextWindowSizeConstraints(minSize, maxSize, overlayTitleHeightConstraint);
        // "##..." renders as an empty native title: the bar keeps its drag
        // rect while the brand row below owns the pixels.
        ImGui::Begin("##vkintox_overlay", nullptr, ImGuiWindowFlags_NoCollapse);
        ImGui::GetStyle().FramePadding.y = previousFramePaddingY;

        const ImVec2 windowPos = ImGui::GetWindowPos();
        const ImVec2 windowSize = ImGui::GetWindowSize();
        const float titleBarHeight = ImGui::GetCurrentWindowRead()->TitleBarHeight;
        const ImGuiM3Metrics& m3metrics = ImGuiM3GetMetrics();
        // three-zone header: icon leading, brand centred, close trailing.
        const float closeButtonSize = 26.0f * m3metrics.density;
        const float closeButtonRightInset = 14.0f * m3metrics.density;
        const ImVec2 buttonMin(windowPos.x + windowSize.x - closeButtonSize - closeButtonRightInset,
                               windowPos.y + (titleBarHeight - closeButtonSize) * 0.5f);
        const ImVec2 buttonMax(buttonMin.x + closeButtonSize, buttonMin.y + closeButtonSize);
        const ImVec2 buttonCenter(buttonMin.x + closeButtonSize * 0.5f,
                                  buttonMin.y + closeButtonSize * 0.5f);
        const bool buttonHovered = ImGui::IsMouseHoveringRect(buttonMin, buttonMax, false);
        const ImVec2 mousePos = ImGui::GetIO().MousePos;
        const bool pointerInTitleBar = mousePos.x >= windowPos.x && mousePos.x < windowPos.x + windowSize.x &&
                                       mousePos.y >= windowPos.y && mousePos.y < windowPos.y + titleBarHeight;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && pointerInTitleBar && !buttonHovered)
        {
            titleRightDragging = true;
            titleRightDragOffsetX = mousePos.x - windowPos.x;
            titleRightDragOffsetY = mousePos.y - windowPos.y;
        }
        if (titleRightDragging)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
                ImGui::SetWindowPos(ImVec2(mousePos.x - titleRightDragOffsetX,
                                           mousePos.y - titleRightDragOffsetY));
            else
                titleRightDragging = false;
        }
        const bool closeRequested = buttonHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        ImDrawList* drawList = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());

        // brand row: icon pinned to the leading edge, wordmark centred on the
        // bar independently of it.
        {
            if (titleIconDescriptor != VK_NULL_HANDLE)
            {
                const float leftPad = 14.0f * m3metrics.density;
                const float iconSize = ImTrunc(titleBarHeight * 0.62f);
                const ImVec2 iconMin(windowPos.x + leftPad, windowPos.y + (titleBarHeight - iconSize) * 0.5f);
                drawList->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(titleIconDescriptor)),
                                   iconMin, ImVec2(iconMin.x + iconSize, iconMin.y + iconSize));
            }
            ImFont* bold = ImGuiM3FontBold();
            const float titleSize = ImGui::GetFontSize() * 1.2f;
            if (bold)
                ImGui::PushFont(bold, titleSize);
            const char* brand = "VKIntox";
            const ImVec2 textSize = ImGui::CalcTextSize(brand);
            drawList->AddText(ImVec2(windowPos.x + (windowSize.x - textSize.x) * 0.5f,
                                     windowPos.y + (titleBarHeight - textSize.y) * 0.5f),
                              ImGuiM3ColorU32(ImGuiM3Role_OnSurface), brand);
            if (bold)
                ImGui::PopFont();
        }

        ImVec4 surfaceColor = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        surfaceColor.w = 1.0f;
        // mask the moving title behind the button, then hover/press on top.
        drawList->AddCircleFilled(buttonCenter, closeButtonSize * 0.5f,
                                  ImGui::ColorConvertFloat4ToU32(surfaceColor));
        if (buttonHovered)
        {
            const ImGuiM3State layer = ImGui::IsMouseDown(ImGuiMouseButton_Left) ? ImGuiM3State_Pressed : ImGuiM3State_Hovered;
            drawList->AddCircleFilled(buttonCenter, closeButtonSize * 0.5f,
                                      ImGuiM3StateLayerU32(ImGuiM3Role_OnSurface, layer));
            ImGui::SetTooltip("Close overlay");
        }
        const ImU32 crossColor = buttonHovered ? ImGuiM3ColorU32(ImGuiM3Role_OnSurface)
                                               : ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant);
        // the `close` glyph is already rounded and evenly weighted.
        static const char kCloseGlyph[] = "\xEE\x85\x8C";
        ImFont* closeIcon = ImGuiM3IconFont();
        if (closeIcon)
        {
            const float px = closeButtonSize * 0.60f;
            ImGuiM3DrawIcon(drawList, kCloseGlyph, ImRect(buttonMin, buttonMax), px, crossColor);
        }
        else
        {
            const float crossInset = closeButtonSize * 0.22f;
            const float crossThickness = 3.0f * m3metrics.density;
            drawList->AddLine(ImVec2(buttonCenter.x - crossInset, buttonCenter.y - crossInset),
                              ImVec2(buttonCenter.x + crossInset, buttonCenter.y + crossInset), crossColor, crossThickness);
            drawList->AddLine(ImVec2(buttonCenter.x + crossInset, buttonCenter.y - crossInset),
                              ImVec2(buttonCenter.x - crossInset, buttonCenter.y + crossInset), crossColor, crossThickness);
        }

        // Clamp position after the window is created (prevents dragging offscreen).
        // the keep-on-screen margin scales with the game window too.
        ImVec2 winPos = ImGui::GetWindowPos();
        ImVec2 winSize = ImGui::GetWindowSize();
        const float keep = ImMin(avail.x, avail.y) * 0.05f;
        bool clamped = false;
        if (winPos.x + winSize.x < screenMin.x + keep) { winPos.x = screenMin.x; clamped = true; }
        if (winPos.y < screenMin.y)                    { winPos.y = screenMin.y; clamped = true; }
        if (winPos.x > screenMax.x - keep)              { winPos.x = screenMax.x - keep; clamped = true; }
        if (winPos.y > screenMax.y - keep)              { winPos.y = screenMax.y - keep; clamped = true; }
        if (clamped)
            ImGui::SetWindowPos(winPos);
        frameInputRects.push_back(InputRect{winPos.x, winPos.y, winSize.x, winSize.y});
        setWaylandInputSurfaceRect(winPos.x, winPos.y, winSize.x, winSize.y);

        // Process shader test (one per frame) regardless of active tab
        processShaderTest();

        // Gather depth-buffer state for the Advanced tab. The overlay shares the
        // process with the layer, so we read LogicalDevice directly under the
        // global lock.
        gatherDepthInfo();
        applyDepthPinRequests();

        // top nav is a connected button group: pill ends, modest shared edges,
        // 2dp inner padding. one control, not five floating tabs. the selected
        // segment swaps to secondary-container and gains weight.
        static int activeView = 0;
        static const char* const kViewLabels[] = {"Effects", "Shaders", "Settings", "Advanced", "Diagnostics", "About"};
        static const ImWchar kViewIcons[] = {Icon::AutoAwesome, Icon::Palette, Icon::Settings, Icon::Tune, Icon::MonitorHeart, Icon::Info};
        constexpr int kViewCount = (int)(sizeof(kViewLabels) / sizeof(kViewLabels[0]));
        ImGui::M3ConnectedButtonGroup("##overlay_nav", kViewLabels, kViewCount, &activeView, kViewIcons);

        // View key: top-level tabs are 3..7, and the Effects tab expands into 0
        // (main), 1 (add effects) and 2 (config manage) so entering and leaving
        // those sub-views slides too.
        auto contentIndex = [&]() -> int {
            if (activeView != 0)
                return activeView + 2;
            if (inSelectionMode)    return 1;
            if (inConfigManageMode) return 2;
            return 0;
        };
        auto renderView = [&](int key) {
            switch (key)
            {
            case 0: renderMainView(keyboard); break;
            case 1: renderAddEffectsView(); break;
            case 2: renderConfigManagerView(); break;
            case 3: renderShaderManagerView(); break;
            case 4: renderSettingsView(keyboard); break;
            case 5: renderAdvancedView(); break;
            case 6: renderDiagnosticsView(); break;
            case 7: renderAboutView(); break;
            default: break;
            }
        };

        // Cross-slide. The outgoing view is drawn stationary and the incoming
        // view is drawn on top of it inside an offset child, so the whole view
        // moves no matter how its own layout places things. The motion is a
        // fixed-duration cubic bezier (the Hyprland curve) sampled from a start
        // timestamp, so it is deterministic: no frame-time integration, and no
        // spring that tightens or bounces depending on how the frames land.
        constexpr float kViewSlideSeconds = 0.30f;
        constexpr float kBezX1 = 0.05f, kBezY1 = 0.90f;
        constexpr float kBezX2 = 0.10f, kBezY2 = 1.12f;

        static int    viewLastIndex     = -1;  // last observed target key
        static int    viewSettledIndex  = 0;   // last fully shown key
        static int    viewTargetIndex   = 0;   // key being shown / slid to
        static int    viewFromIndex     = 0;   // key sliding out
        static int    viewSlideDir      = 1;
        static bool   viewTransitioning = false;
        static double viewSlideStart    = 0.0;

        const int viewIndex = contentIndex();
        if (viewIndex != viewLastIndex)
        {
            if (viewLastIndex >= 0)
            {
                // Always slide from the last settled view. Rapid re-targeting
                // just restarts settled -> newest instead of composing two
                // half-finished transitions, which is what made fast switching
                // glitchy.
                viewFromIndex     = viewSettledIndex;
                viewTargetIndex   = viewIndex;
                viewTransitioning = (viewFromIndex != viewTargetIndex);
                viewSlideDir      = (viewIndex > viewSettledIndex) ? 1 : -1;
                viewSlideStart    = ImGui::GetTime();
            }
            else
            {
                viewSettledIndex = viewIndex;
                viewTargetIndex  = viewIndex;
            }
            viewLastIndex = viewIndex;
        }

        float viewEase = 1.0f;
        if (viewTransitioning)
        {
            const float clock = static_cast<float>((ImGui::GetTime() - viewSlideStart) / kViewSlideSeconds);
            if (clock >= 1.0f)
            {
                viewTransitioning = false;
                viewSettledIndex = viewTargetIndex;
            }
            else
            {
                viewEase = CubicBezierEase(clock, kBezX1, kBezY1, kBezX2, kBezY2);
            }
        }

        const ImVec2 viewPos  = ImGui::GetCursorScreenPos();
        const ImVec2 viewSize = ImGui::GetContentRegionAvail();
        const float slideOffset = (1.0f - viewEase) * static_cast<float>(viewSlideDir) * viewSize.x;

        // Clip the whole strip to the content region so a sliding view can never
        // draw over the nav or the title bar.
        ImGui::PushClipRect(viewPos, ImVec2(viewPos.x + viewSize.x, viewPos.y + viewSize.y), true);

        if (viewTransitioning && viewFromIndex != viewTargetIndex)
        {
            ImGui::SetCursorScreenPos(viewPos);
            renderView(viewFromIndex);
        }

        // Incoming view on top, offset. A zero-padding child is what guarantees
        // the offset applies even for views that position their own content; its
        // opaque background makes it slide *over* the outgoing view instead of
        // showing it through. The style vars are popped right after BeginChild
        // so popups opened inside the view still get the real window padding.
        ImGui::SetCursorScreenPos(ImVec2(viewPos.x + slideOffset, viewPos.y));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImVec4 viewBg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        viewBg.w = 1.0f;  // opaque, so the incoming view fully covers the outgoing one
        ImGui::PushStyleColor(ImGuiCol_ChildBg, viewBg);
        ImGui::BeginChild("##view_slide", viewSize, false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        renderView(viewTargetIndex);
        ImGui::EndChild();

        ImGui::PopClipRect();
        ImGui::SetCursorScreenPos(viewPos);

        ImGui::End();  // VKIntox Overlay

        if (closeRequested)
            toggle();

        // Debug window (separate, controlled by setting)
        renderDebugWindow();

        // toasts last, so they draw above the overlay and debug windows
        renderToasts();

        // Global auto-apply check (runs regardless of which tab is active)
        if (settingsManager.getAutoApply() && paramsDirty)
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastChangeTime).count();
            const bool mouseButtonDown = ImGui::GetIO().MouseDown[0] || ImGui::GetIO().MouseDown[1];

            // Defer auto-apply while either mouse button is held (dragging/resizing/sliders).
            // Apply on the first frame after release once delay has already elapsed.
            if (elapsed >= settingsManager.getAutoApplyDelay() && !mouseButtonDown)
            {
                // a ReShade uniform edit is already live via updateEffect, so
                // only rebuild the chain when something structural changed
                if (reloadNeeded)
                    applyRequested = true;
                reloadNeeded = false;
                paramsDirty = false;
                profileDirty = true;  // Mark for auto-save to profile
            }
        }

        // Auto-save profile when changes are applied
        if (profileDirty && !paramsDirty &&
            (!activeProfilePath.empty() || !activeShaderProfilePath.empty()))
            autoSaveProfile();

        publishOverlayInput();
        ImGui::Render();

        // Begin render pass
        VkRenderPassBeginInfo rpBegin = {};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = renderPass;
        rpBegin.framebuffer = framebuffer;
        rpBegin.renderArea.extent.width = width;
        rpBegin.renderArea.extent.height = height;

        logicalDevice->vkd.CmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        logicalDevice->vkd.CmdEndRenderPass(cmd);

        vr = logicalDevice->vkd.EndCommandBuffer(cmd);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to end ImGui command buffer: " + std::to_string(vr));
            return VK_NULL_HANDLE;
        }

        return cmd;
    }

    void ImGuiOverlay::collectCommandFences(std::vector<VkFence>& out) const
    {
        for (VkFence f : commandBufferFences)
            if (f != VK_NULL_HANDLE) out.push_back(f);
    }

} // namespace VKIntox
