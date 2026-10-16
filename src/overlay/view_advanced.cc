#include "imgui_overlay.hh"
#include "settings_manager.hh"
#include "vkintox_internal.hh"
#include "logger.hh"
#include "overlay/ui_theme.hh"
#include "overlay/ui_icons.hh"
#include "overlay/ui_menu.hh"

#include <algorithm>
#include <sstream>
#include <atomic>

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imgui_internal.h"

namespace VKIntox
{
    // Dirty flag for deferred saving — avoids blocking the UI thread with
    // synchronous disk I/O on every single checkbox/radio/combo change.
    // The actual save happens once per frame in renderAdvancedView().
    static std::atomic<bool> g_settingsDirty{false};
    
    // Mark settings as needing save (call from UI callbacks)
    static inline void markSettingsDirty()
    {
        g_settingsDirty.store(true, std::memory_order_relaxed);
    }
    
    // Perform deferred save if dirty (call once per frame)
    static inline void flushSettingsSaveIfNeeded()
    {
        if (g_settingsDirty.exchange(false, std::memory_order_acq_rel))
        {
            settingsManager.save();
        }
    }

    // window-sized first (exact or titlebar-inset), then the swapchain-linked
    // candidate; the busiest buffer can be the wrong size
    static VkImageView defaultPinCandidateForSwapchain(const DepthInfo& info)
    {
        constexpr int64_t kChromeTolerancePx = 128;
        auto windowSized = [&](const DepthCandidateInfo& c) {
            const int64_t dw = static_cast<int64_t>(c.extent.width)  - static_cast<int64_t>(info.swapchainExtent.width);
            const int64_t dh = static_cast<int64_t>(c.extent.height) - static_cast<int64_t>(info.swapchainExtent.height);
            return dw >= -kChromeTolerancePx && dw <= kChromeTolerancePx
                && dh >= -kChromeTolerancePx && dh <= kChromeTolerancePx;
        };

        for (const auto& c : info.candidates)
            if (c.extent.width == info.swapchainExtent.width
                && c.extent.height == info.swapchainExtent.height)
                return c.imageView;
        for (const auto& c : info.candidates)
            if (windowSized(c))
                return c.imageView;
        for (const auto& c : info.candidates)
            if (c.hasPresentableSnapshotTarget)
                return c.imageView;
        return info.candidates.empty() ? VK_NULL_HANDLE : info.candidates.front().imageView;
    }

    // Convert a VkFormat to a short human-readable string for the UI.
    static const char* depthFormatName(VkFormat format)
    {
        switch (format)
        {
        case VK_FORMAT_D16_UNORM:           return "D16_UNORM";
        case VK_FORMAT_X8_D24_UNORM_PACK32: return "X8_D24_PACK32";
        case VK_FORMAT_D32_SFLOAT:          return "D32_SFLOAT";
        case VK_FORMAT_D16_UNORM_S8_UINT:   return "D16_UNORM_S8_UINT";
        case VK_FORMAT_D24_UNORM_S8_UINT:   return "D24_UNORM_S8_UINT";
        case VK_FORMAT_D32_SFLOAT_S8_UINT:  return "D32_SFLOAT_S8_UINT";
        case VK_FORMAT_R32_SFLOAT:          return "R32_SFLOAT (resolved)";
        case VK_FORMAT_UNDEFINED:           return "none";
        default:                            return "other";
        }
    }

    static const char* layoutName(VkImageLayout layout)
    {
        switch (layout)
        {
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DS_ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:         return "D_ATTACHMENT_OPTIMAL";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:  return "DS_READ_ONLY_OPTIMAL";
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:          return "D_READ_ONLY_OPTIMAL";
        case VK_IMAGE_LAYOUT_GENERAL:                          return "GENERAL";
        case VK_IMAGE_LAYOUT_UNDEFINED:                        return "undefined";
        default:                                               return "other";
        }
    }

    static const char* sampleName(VkSampleCountFlagBits samples)
    {
        switch (samples)
        {
        case VK_SAMPLE_COUNT_1_BIT:  return "1x";
        case VK_SAMPLE_COUNT_2_BIT:  return "2x";
        case VK_SAMPLE_COUNT_4_BIT:  return "4x";
        case VK_SAMPLE_COUNT_8_BIT:  return "8x";
        case VK_SAMPLE_COUNT_16_BIT: return "16x";
        case VK_SAMPLE_COUNT_32_BIT: return "32x";
        case VK_SAMPLE_COUNT_64_BIT: return "64x";
        default:                     return "?";
        }
    }

    void ImGuiOverlay::gatherDepthInfo()
    {
        if (!logicalDevice)
            return;

        std::lock_guard<std::mutex> l(globalLock);

        depthInfo.supportedResolveModes = logicalDevice->supportedDepthResolveModes;
        depthInfo.depthCaptureEnabled   = settingsManager.getDepthCapture();
        depthInfo.swapchainExtent       = currentSwapchainExtentLocked(logicalDevice);

        deviceInfo.gpuName      = logicalDevice->gpuName;
        deviceInfo.gpuDriverInfo = logicalDevice->gpuDriverInfo;
        deviceInfo.gpuPciSlot   = logicalDevice->gpuPciSlot;
        deviceInfo.gpuApiVersion = logicalDevice->gpuApiVersion;
        deviceInfo.gpuVendorId  = logicalDevice->gpuVendorId;

        const int modePref = settingsManager.getDepthResolveMode();
        const bool avgSupported = (logicalDevice->supportedDepthResolveModes & VK_RESOLVE_MODE_AVERAGE_BIT) != 0;
        if (modePref == 2 && avgSupported)
            depthInfo.depthResolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        else
            depthInfo.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;

        // Snapshot pin state from the device.  If the pinned view was destroyed
        // by the render thread since last frame, clear the stale pin here so the
        // UI immediately falls back to auto instead of showing a ghost pin.
        depthInfo.pinnedView = logicalDevice->pinnedDepthImageView;
        depthInfo.depthIsPinned = (depthInfo.pinnedView != VK_NULL_HANDLE);

        if (depthInfo.depthIsPinned)
        {
            auto it = logicalDevice->depthViewStates.find(depthInfo.pinnedView);
            if (it != logicalDevice->depthViewStates.end())
            {
                // Pinned view is still valid — show it as the active buffer.
                const DepthState& pinned = it->second;
                depthInfo.active.imageView       = pinned.imageView;
                depthInfo.active.format          = pinned.format;
                depthInfo.active.extent          = pinned.extent;
                depthInfo.active.samples         = pinned.samples;
                depthInfo.active.observedLayout  = pinned.observedLayout;
                depthInfo.active.transient       = pinned.transient;
                depthInfo.active.drawCount = logicalDevice->bestDepthCandidate.valid
                                              && logicalDevice->bestDepthCandidate.depthState.imageView == pinned.imageView
                                              ? logicalDevice->bestDepthCandidate.drawCount : 0;
                depthInfo.active.hasPresentableSnapshotTarget =
                    logicalDevice->bestDepthCandidate.valid
                    && logicalDevice->bestDepthCandidate.depthState.imageView == pinned.imageView
                    ? logicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget : false;
            }
            else
            {
                // Pinned view was destroyed since last frame — clear the stale pin
                // so we fall back to auto.  getDepthState() in vkintox.cpp does the
                // same check, but we need it here too so the UI is consistent.
                Logger::debug("gatherDepthInfo: pinned view no longer tracked, clearing stale pin");
                logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
                depthInfo.depthIsPinned = false;
                depthInfo.pinnedView    = VK_NULL_HANDLE;
                // Don't set active here — fall through to the auto path below.
            }
        }

        // Auto path (also handles the case where the pin was just cleared above).
        if (!depthInfo.depthIsPinned)
        {
            const DepthState& active = logicalDevice->activeDepthState;
            depthInfo.active.imageView       = active.imageView;
            depthInfo.active.format          = active.format;
            depthInfo.active.extent          = active.extent;
            depthInfo.active.samples         = active.samples;
            depthInfo.active.observedLayout  = active.observedLayout;
            depthInfo.active.transient       = active.transient;
            depthInfo.active.drawCount       = logicalDevice->bestDepthCandidate.valid
                                                 ? logicalDevice->bestDepthCandidate.drawCount : 0;
            depthInfo.active.hasPresentableSnapshotTarget =
                logicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget;
        }
        depthInfo.depthResolveIsMsaa = depthInfo.active.samples != VK_SAMPLE_COUNT_1_BIT;

        // Populate candidate list from all tracked depth views.
        depthInfo.candidates.clear();
        for (const auto& [view, ds] : logicalDevice->depthViewStates)
        {
            DepthCandidateInfo c;
            c.imageView      = view;
            c.format         = ds.format;
            c.extent         = ds.extent;
            c.samples        = ds.samples;
            c.observedLayout = ds.observedLayout;
            c.transient      = ds.transient;
            if (logicalDevice->bestDepthCandidate.valid &&
                logicalDevice->bestDepthCandidate.depthState.imageView == view)
            {
                c.drawCount = logicalDevice->bestDepthCandidate.drawCount;
                c.hasPresentableSnapshotTarget =
                    logicalDevice->bestDepthCandidate.hasPresentableSnapshotTarget;
            }
            depthInfo.candidates.push_back(c);
        }
        // Sort by drawCount descending so the best candidate appears first.
        std::sort(depthInfo.candidates.begin(), depthInfo.candidates.end(),
                  [](const DepthCandidateInfo& a, const DepthCandidateInfo& b){
                      return a.drawCount > b.drawCount;
                  });
    }

    void ImGuiOverlay::applyDepthPinRequests()
    {
        if (!logicalDevice)
            return;

        if (depthPinPendingView != VK_NULL_HANDLE)
        {
            std::lock_guard<std::mutex> l(globalLock);
            auto it = logicalDevice->depthViewStates.find(depthPinPendingView);
            if (it != logicalDevice->depthViewStates.end())
            {
                logicalDevice->pinnedDepthImageView = depthPinPendingView;
                Logger::info("depth manual pin set to view 0x" +
                    std::to_string(reinterpret_cast<uintptr_t>(depthPinPendingView)));
                depthPinChanged = true;
                // Force deferred realloc so descriptor sets + MSAA framebuffers
                // get rebuilt for the new depth source view.
                logicalDevice->depthReallocPending = true;
            }
            else
            {
                Logger::warn("depth pin requested but view not found (may have been destroyed)");
            }
            depthPinPendingView = VK_NULL_HANDLE;
        }

        if (depthPinPendingClear)
        {
            std::lock_guard<std::mutex> l(globalLock);
            logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
            depthPinPendingClear = false;
            depthPinChanged = true;
            // Same: force realloc so we stop using the just-unpinned view.
            logicalDevice->depthReallocPending = true;
            Logger::info("depth manual pin cleared (back to auto-promotion)");
        }

        if (depthTransformPending)
        {
            std::lock_guard<std::mutex> l(globalLock);
            // the resolve owns transforms now (macros no longer carry them),
            // so re-recording it is all a toggle needs
            logicalDevice->depthReallocPending = true;
            depthTransformPending = false;
        }
    }

    // The one-line describer under the card's hero header. Reads as a sentence
    // instead of the old column dump: 1920x1080 (D16_UNORM, MSAA 1x, ...).
    static std::string describeActiveDepth(const DepthInfo& info)
    {
        if (info.active.imageView == VK_NULL_HANDLE)
            return "No depth buffer detected yet";

        std::string s = std::to_string(info.active.extent.width) + "x" +
                        std::to_string(info.active.extent.height);
        s += " (";
        s += depthFormatName(info.active.format);
        s += ", MSAA ";
        s += sampleName(info.active.samples);
        s += info.depthIsPinned ? ", pinned manually" : ", picked automatically";
        s += ")";
        return s;
    }

    namespace
    {
        struct DepthRowAction
        {
            VkImageView pin = VK_NULL_HANDLE;
            bool        unpin = false;
        };

        // One reported buffer drawn as an expressive list row: index disc, size on
        // the document face, format / samples / draws beneath, a status pill and a
        // pin action on the trailing edge. Clicking the row pins it too, so the
        // whole surface is a target.
        DepthRowAction drawDepthBufferRow(int index, const DepthCandidateInfo& c,
                                          bool isActive, bool isPinned, float rowH, float rowGap, bool first)
        {
            DepthRowAction action;
            const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
            const float d = m.density;
            ImGui::PushID(index);
            if (!first)
                ImGui::Dummy(ImVec2(0.0f, rowGap));

            const float rowW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Header, 0);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, 0);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, 0);
            const ImGuiSelectableFlags rowFlags = (ImGuiSelectableFlags)(
                (int)ImGuiSelectableFlags_AllowOverlap | (int)ImGuiSelectableFlags_NoPadWithHalfSpacing);
            const bool rowPressed = ImGui::Selectable("##depth_row", false, rowFlags, ImVec2(rowW, rowH));
            ImGui::PopStyleColor(3);
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 mn = ImGui::GetItemRectMin();
            const ImVec2 mx = ImGui::GetItemRectMax();
            const ImRect rowRect(mn, mx);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImFont* brand = ImGuiM3FontBold();
            ImFont* bodyBold = ImGuiM3FontBodyBold();

            const float radius = ImGuiM3Radius(ImGuiM3Shape_Medium);
            const ImGuiM3ShapeRounding round{ radius, radius, radius, radius };
            if (isActive)
                ImGuiM3PathRoundedRect(dl, rowRect, round, ImGuiM3ColorU32(ImGuiM3Role_SecondaryContainer));
            else if (isPinned)
                ImGuiM3PathRoundedRect(dl, rowRect, round, ImGuiM3ColorU32(ImGuiM3Role_PrimaryContainer));
            else
            {
                ImGuiM3PathRoundedRect(dl, rowRect, round, ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainer));
                if (hovered)
                    ImGuiM3DrawStateLayer(dl, rowRect, round, ImGuiM3Role_OnSurface, ImGuiM3State_Hovered);
            }

            const ImU32 titleCol = isActive ? ImGuiM3ColorU32(ImGuiM3Role_OnSecondaryContainer)
                                            : ImGuiM3ColorU32(ImGuiM3Role_OnSurface);
            const ImU32 supportCol = isActive ? ImGuiM3ColorU32(ImGuiM3Role_OnSecondaryContainer)
                                              : ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant);

            // leading index disc
            const float disc = 26.0f * d;
            const ImVec2 discCenter(mn.x + 14.0f * d + disc * 0.5f, mn.y + rowH * 0.5f);
            dl->AddCircleFilled(discCenter, disc * 0.5f,
                                isActive ? ImGuiM3ColorU32(ImGuiM3Role_Primary)
                                         : ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainerHighest), 24);
            const std::string indexText = std::to_string(index);
            const float indexPx = 11.0f * d;
            ImGui::PushFont(bodyBold, indexPx);
            const ImVec2 indexSize = ImGui::CalcTextSize(indexText.c_str());
            ImGui::PopFont();
            dl->AddText(bodyBold, indexPx,
                        ImVec2(discCenter.x - indexSize.x * 0.5f, discCenter.y - indexSize.y * 0.5f),
                        isActive ? ImGuiM3ColorU32(ImGuiM3Role_OnPrimary)
                                 : ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant),
                        indexText.c_str());

            const std::string sizeText = std::to_string(c.extent.width) + "x" + std::to_string(c.extent.height);
            std::string detail = depthFormatName(c.format);
            detail += "  \xC2\xB7  MSAA ";
            detail += sampleName(c.samples);
            detail += "  \xC2\xB7  ";
            detail += std::to_string(c.drawCount) + " draws";
            if (c.transient)
                detail += "  \xC2\xB7  transient";
            if (c.hasPresentableSnapshotTarget)
                detail += "  \xC2\xB7  swapchain";

            const float titlePx = 14.0f * d;
            const float detailPx = 11.0f * d;
            const float textX = mn.x + 14.0f * d + disc + 12.0f * d;
            ImGui::PushFont(brand, titlePx);
            const ImVec2 titleSize = ImGui::CalcTextSize(sizeText.c_str());
            ImGui::PopFont();
            ImGui::PushFont(bodyBold, detailPx);
            const ImVec2 detailSize = ImGui::CalcTextSize(detail.c_str());
            ImGui::PopFont();
            const float textBlockH = titleSize.y + detailSize.y + 1.0f * d;
            const float textY = mn.y + (rowH - textBlockH) * 0.5f;
            dl->AddText(brand, titlePx, ImVec2(textX, textY), titleCol, sizeText.c_str());
            dl->AddText(bodyBold, detailPx, ImVec2(textX, textY + titleSize.y + 1.0f * d), supportCol, detail.c_str());

            // status chip: right beside the resolution, so the eye reads
            // "1920x1080 PINNED" as one fact rather than scanning to the action
            if (isActive || isPinned)
            {
                const char* badge = isPinned ? "PINNED" : "ACTIVE";
                const float badgePx = 9.0f * d;
                ImGui::PushFont(bodyBold, badgePx);
                const ImVec2 badgeText = ImGui::CalcTextSize(badge);
                ImGui::PopFont();
                const float badgeH = 15.0f * d;
                const float badgeR = badgeH * 0.5f;
                const ImVec2 badgeMin(textX + titleSize.x + 8.0f * d,
                                      textY + (titleSize.y - badgeH) * 0.5f);
                const ImVec2 badgeMax(badgeMin.x + badgeText.x + 12.0f * d, badgeMin.y + badgeH);
                ImGuiM3PathRoundedRect(dl, ImRect(badgeMin, badgeMax),
                                       ImGuiM3ShapeRounding{ badgeR, badgeR, badgeR, badgeR },
                                       ImGuiM3ColorU32(isPinned ? ImGuiM3Role_Primary
                                                                : ImGuiM3Role_PrimaryContainer));
                dl->AddText(bodyBold, badgePx,
                            ImVec2(badgeMin.x + 6.0f * d, badgeMin.y + (badgeH - badgeText.y) * 0.5f),
                            ImGuiM3ColorU32(isPinned ? ImGuiM3Role_OnPrimary
                                                     : ImGuiM3Role_OnPrimaryContainer),
                            badge);
            }

            // trailing pin action
            const float btnH = 32.0f * d;
            const float btnW = 70.0f * d;
            const float btnY = mn.y + (rowH - btnH) * 0.5f;
            const float rightX = mx.x - 10.0f * d;

            ImGui::SetCursorScreenPos(ImVec2(rightX - btnW, btnY));
            if (ImGui::M3Button(isPinned ? "Unpin" : "Pin",
                                isPinned ? ImGuiM3Button_Text : ImGuiM3Button_Tonal,
                                ImVec2(btnW, btnH)))
            {
                if (isPinned)
                    action.unpin = true;
                else
                    action.pin = c.imageView;
            }
            ImGui::SetCursorScreenPos(ImVec2(mn.x, mx.y));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));

            if (rowPressed)
            {
                if (isPinned)
                    action.unpin = true;
                else
                    action.pin = c.imageView;
            }

            if (hovered)
            {
                ImGui::BeginTooltip();
                ImGui::Text("View:      0x%llx", reinterpret_cast<unsigned long long>(c.imageView));
                ImGui::Text("Layout:    %s", layoutName(c.observedLayout));
                ImGui::Text("Draws:     %u", c.drawCount);
                ImGui::Text("Transient: %s", c.transient ? "yes" : "no");
                ImGui::TextColored(UI::Success(), "Click a row to pin it.");
                ImGui::EndTooltip();
            }

            ImGui::PopID();
            return action;
        }
    } // namespace

    void ImGuiOverlay::renderAdvancedView()
    {
        const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
        const float d = m.density;

        // --- Hero: the icon+label pairing, master capture switch on the row ---
        //
        // the switch only records intent: the hooks arm at device creation, so a
        // live flip would re-arm them mid-frame. the boot value is captured on the
        // first draw, and anything else is waiting on a restart.
        static const bool s_depthCaptureAtStartup = settingsManager.getDepthCapture();
        bool depthCapture = settingsManager.getDepthCapture();
        if (ImGui::M3HeroSwitch(Icon::LayersUtf8, "Depth buffer", "##depth_capture", &depthCapture,
                                "Capture the depth buffer for depth-based effects.\n"
                                "Saved now, applied on the next launch: detection arms at startup.",
                                1.15f))
        {
            settingsManager.setDepthCapture(depthCapture);
            markSettingsDirty();
            Logger::info(std::string("depth capture ") + (depthCapture ? "enabled" : "disabled") +
                         " (saved; takes effect on restart)");
            pushToast(LogLevel::Info, depthCapture
                ? "Depth buffer capture on. Restart to apply."
                : "Depth buffer capture off. Restart to apply.");
        }
        // status describer: smaller, dimmed, body bold, tucked under the label
        {
            UI::PushSupport();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 72.0f * d);
            ImGui::TextDisabled("%s", describeActiveDepth(depthInfo).c_str());
            UI::PopSupport();
        }
        if (depthCapture != s_depthCaptureAtStartup)
        {
            // under the switch it belongs to
            const float notePx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodySmall).size * d;
            ImFont* noteFont = ImGuiM3FontBodyBold();
            ImGui::PushFont(noteFont, notePx);
            const ImVec2 noteSize = ImGui::CalcTextSize("Requires restart");
            ImGui::PopFont();
            const float noteX = ImGui::GetWindowWidth() - noteSize.x -
                                ImGui::GetCurrentWindowRead()->WindowPadding.x;
            ImGui::SetCursorPosX(noteX);
            ImGui::BeginDisabled();
            ImGui::PushFont(noteFont, notePx);
            ImGui::TextUnformatted("Requires restart");
            ImGui::PopFont();
            ImGui::EndDisabled();
        }

        ImGui::Spacing();
        // --- Auto / Manual pin ---
        {
            const char* modes[] = { "Auto", "Manual pin" };
            const char* hints[] = {
                "Auto promotes the buffer with the most draws, or a swapchain-linked one.",
                "Manual pin starts from the buffer matching the window size; pick another from the list."
            };
            int mode = depthInfo.depthIsPinned ? 1 : 0;
            int hovered_segment = -1;
            if (ImGui::M3ConnectedButtonGroup("##depth_mode", modes, 2, &mode, NULL, false, &hovered_segment))
            {
                if (mode == 0)
                {
                    if (depthInfo.depthIsPinned)
                        depthPinPendingClear = true;
                }
                else if (!depthInfo.candidates.empty())
                {
                    // unbiassed: the first reported buffer, so the switch is
                    // immediate even before anything has been drawn this frame.
                    depthPinPendingView = defaultPinCandidateForSwapchain(depthInfo);
                }
                else
                {
                    Logger::info("manual pin requested but no depth buffers are tracked yet");
                }
            }
            if (hovered_segment >= 0)
                ImGui::SetTooltip("%s", hints[hovered_segment]);
        }

        ImGui::Spacing();
        // --- Reported buffers live on their own page, pushed from here ---
        {
            const std::string trailing = std::to_string(depthInfo.candidates.size());
            if (UI::SubViewRow("Reported depth buffers", trailing.c_str(), Icon::GridViewUtf8))
                openSubView = SubView_DepthBuffers;
        }

        ImGui::Spacing();
        // --- Encoding: the two modes that actually behave differently ---
        // Luminance/Red and every other single-channel mask decode depth the same
        // way, so they collapse to "Red"; View-space Z is the only transform with
        // distinct behaviour.
        {
            static const int kChannels[] = { 0, 4 };   // depth source channel indices
            const char* labels[] = { "Luminance/Red", "View-space Z" };
            const char* hints[] = {
                "Read depth from the red / luminance channel. The standard Vulkan depth layout.",
                "Convert raw view-space Z into [0,1]. Use when the red channel reads flat or inverted."
            };
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Encoding");
            ImGui::SameLine(150.0f);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::PushID("##depth_encoding");
            int current = settingsManager.getDepthSourceChannel();
            int index = (current == kChannels[1]) ? 1 : 0;
            int hovered_segment = -1;
            if (ImGui::M3ConnectedButtonGroup("##depth_channel", labels, 2, &index, NULL, false, &hovered_segment))
            {
                settingsManager.setDepthSourceChannel(kChannels[index]);
                depthTransformPending = true;
                markSettingsDirty();
            }
            if (hovered_segment >= 0)
                ImGui::SetTooltip("%s", hints[hovered_segment]);
            ImGui::PopID();
        }

        // --- Transforms: one compact row ---
        ImGui::Spacing();
        {
            bool depthInvert = settingsManager.getDepthInvert();
            bool depthFlip   = settingsManager.getDepthFlip();
            bool depthFlop   = settingsManager.getDepthFlop();
            bool changed = false;

            if (ImGui::Checkbox("Invert", &depthInvert))
            {
                settingsManager.setDepthInvert(depthInvert);
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Invert: depth = 1.0 - depth. Flips the near/far interpretation.");

            ImGui::SameLine(0.0f, 18.0f * d);
            if (ImGui::Checkbox("Flip", &depthFlip))
            {
                settingsManager.setDepthFlip(depthFlip);
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Mirror the depth image vertically (uv.y -> 1 - uv.y).");

            ImGui::SameLine(0.0f, 18.0f * d);
            if (ImGui::Checkbox("Mirror", &depthFlop))
            {
                settingsManager.setDepthFlop(depthFlop);
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Mirror the depth image horizontally (uv.x -> 1 - uv.x).");

            if (changed)
            {
                depthTransformPending = true;
                markSettingsDirty();
            }
        }

        // --- Rebuild timing: what the refresh button below and every transform use ---
        ImGui::Spacing();
        {
            const char* labels[] = { "Safe", "Fast" };
            const char* hints[] = {
                "Idle the GPU before rebuilding the depth resolve path, so nothing in flight is still reading what gets freed.",
                "Rebuild behind the layer's own submit fences without idling. Reacts a frame sooner; a driver can fault if the app still references an old resolve target."
            };
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Rebuild");
            ImGui::SameLine(150.0f);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::PushID("##depth_rebuild");
            int index = (settingsManager.getDepthRebuildMode() == 0) ? 0 : 1;
            int hovered_segment = -1;
            if (ImGui::M3ConnectedButtonGroup("##depth_rebuild_mode", labels, 2, &index, NULL, false, &hovered_segment))
            {
                settingsManager.setDepthRebuildMode(index);
                markSettingsDirty();
            }
            if (hovered_segment >= 0)
                ImGui::SetTooltip("%s", hints[hovered_segment]);
            ImGui::PopID();
        }

        // --- Footer ---
        ImGui::Spacing();
        const std::string redetectLabel = std::string(Icon::RefreshUtf8) + "  Refresh depth buffers";
        if (ImGui::M3Button(redetectLabel.c_str(), ImGuiM3Button_Tonal))
        {
            std::lock_guard<std::mutex> l(globalLock);
            logicalDevice->activeDepthState    = DepthState{};
            logicalDevice->bestDepthCandidate  = LogicalDevice::DepthCandidateTrackingState{};
            logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
            depthPinChanged = true;
            Logger::info("depth re-detect requested from Advanced UI");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Clear the active buffer, the best candidate and any pin.\nThe layer re-evaluates every render pass next frame.");

        // --- Deferred save ---
        flushSettingsSaveIfNeeded();
    }

    // --- Pushed page: every tracked depth buffer -------------------------------
    void ImGuiOverlay::renderDepthBuffersView()
    {
        const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
        const float d = m.density;

        if (UI::BreadcrumbBar("Go back", Icon::ChevronLeftUtf8))
        {
            openSubView = SubView_None;
            return;
        }

        ImGui::Spacing();
        ImGui::M3HeroHeader(Icon::LayersUtf8, "Reported depth buffers", describeActiveDepth(depthInfo).c_str());
        ImGui::Spacing();

        // mode control, so the page stands on its own
        {
            const char* modes[] = { "Auto", "Manual pin" };
            const char* hints[] = {
                "Auto promotes the buffer with the most draws, or a swapchain-linked one.",
                "Manual pin starts from the buffer matching the window size; pick another from the list."
            };
            int mode = depthInfo.depthIsPinned ? 1 : 0;
            int hovered_segment = -1;
            if (ImGui::M3ConnectedButtonGroup("##depth_page_mode", modes, 2, &mode, NULL, false, &hovered_segment))
            {
                if (mode == 0)
                {
                    if (depthInfo.depthIsPinned)
                        depthPinPendingClear = true;
                }
                else if (!depthInfo.candidates.empty())
                {
                    depthPinPendingView = defaultPinCandidateForSwapchain(depthInfo);
                }
            }
            if (hovered_segment >= 0)
                ImGui::SetTooltip("%s", hints[hovered_segment]);
        }

        ImGui::Spacing();
        if (depthInfo.candidates.empty())
        {
            UI::TextSupport("No depth views tracked yet. Render a frame with depth.");
            flushSettingsSaveIfNeeded();
            return;
        }

        const float rowH = 52.0f * d;
        const float rowGap = 6.0f * d;
        ImGui::BeginChild("##depth_page_list", ImVec2(0, 0), false);
        for (size_t i = 0; i < depthInfo.candidates.size(); ++i)
        {
            const DepthCandidateInfo& c = depthInfo.candidates[i];
            const bool isActive = (c.imageView == depthInfo.active.imageView);
            const bool isPinned = depthInfo.depthIsPinned && (c.imageView == depthInfo.pinnedView);
            const DepthRowAction action = drawDepthBufferRow(static_cast<int>(i), c, isActive, isPinned,
                                                             rowH, rowGap, i == 0);
            if (action.pin != VK_NULL_HANDLE)
                depthPinPendingView = action.pin;
            else if (action.unpin)
                depthPinPendingClear = true;
        }
        ImGui::EndChild();

        flushSettingsSaveIfNeeded();
    }

} // namespace VKIntox
