#include "imgui_overlay.hh"
#include "settings_manager.hh"
#include "logger.hh"
#include "overlay/ui_theme.hh"
#include "overlay/ui_icons.hh"

#include <algorithm>
#include <sstream>
#include <atomic>

#include "vendor/imgui/imgui.h"

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
    }

    void ImGuiOverlay::renderAdvancedView()
    {
        ImGui::BeginChild("AdvancedContent", ImVec2(0, 0), false);

        ImGui::TextDisabled("Depth buffer capture, decoding and selection.");
        if (!depthInfo.depthCaptureEnabled)
        {
            ImGui::Spacing();
            ImGui::TextColored(UI::Warning(), "%s  Depth capture is OFF — enable it in Settings (requires restart).", Icon::WarningUtf8);
        }
        ImGui::Spacing();

        // --- Status ---
        ImGui::M3CardBegin("adv_status", "Status", Icon::LayersUtf8);
        if (depthInfo.active.imageView == VK_NULL_HANDLE)
        {
            ImGui::TextDisabled("No depth buffer detected yet.");
            ImGui::TextDisabled("Render a frame with depth to populate this.");
        }
        else
        {
            ImGui::Text("%s  %s  %ux%u  %s",
                depthInfo.depthIsPinned ? "PINNED" : "AUTO",
                depthFormatName(depthInfo.active.format),
                depthInfo.active.extent.width, depthInfo.active.extent.height,
                sampleName(depthInfo.active.samples));
            if (depthInfo.depthResolveIsMsaa)
            {
                const char* mode = (depthInfo.depthResolveMode == VK_RESOLVE_MODE_AVERAGE_BIT) ? "average" : "sample-zero";
                ImGui::SameLine();
                ImGui::TextColored(UI::Secondary(), "[MSAA: %s]", mode);
            }
            if (depthInfo.depthIsPinned)
            {
                ImGui::SameLine();
                ImGui::TextColored(UI::Warning(), "[PINNED]");
            }
            ImGui::Spacing();
            ImGui::TextDisabled("HW resolve modes: 0x%x", depthInfo.supportedResolveModes);
        }
        ImGui::M3CardEnd();

        // --- Selection ---
        ImGui::Spacing();
        ImGui::M3CardBegin("adv_select", "Selection", Icon::FilterAltUtf8);
        const bool isPinned = depthInfo.depthIsPinned;

        if (ImGui::RadioButton("Auto (best candidate)", !isPinned))
        {
            if (isPinned)
                depthPinPendingClear = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Automatically pick the depth buffer with the most draws\nor a swapchain-linked snapshot target.");

        ImGui::SameLine();
        if (ImGui::RadioButton("Manual pin", isPinned))
        {
            // No-op: pin a specific row below to set it.
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Pin a specific depth buffer from the list below.\nIf the pinned buffer is destroyed, falls back to auto.");

        if (isPinned)
        {
            ImGui::Spacing();
            ImGui::TextColored(UI::Warning(), "Pinned: %s  %ux%u  %s",
                depthFormatName(depthInfo.active.format),
                depthInfo.active.extent.width, depthInfo.active.extent.height,
                sampleName(depthInfo.active.samples));
            ImGui::SameLine();
            const std::string clearLabel = std::string(Icon::CloseUtf8) + "  Clear Pin";
            if (ImGui::Button(clearLabel.c_str()))
                depthPinPendingClear = true;
        }
        ImGui::M3CardEnd();

        // --- Tracked depth buffers ---
        ImGui::Spacing();
        if (!depthInfo.candidates.empty())
        {
            ImGui::M3CardBegin("adv_buffers", "Reported depth buffers", Icon::GridViewUtf8);
            if (ImGui::BeginTable("##depth_tbl", 5,
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg,
                                  ImVec2(0, 0)))
            {
                ImGui::TableSetupColumn("#",      ImGuiTableColumnFlags_WidthFixed, 32.0f);
                ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthStretch, 0.9f);
                ImGui::TableSetupColumn("Size",   ImGuiTableColumnFlags_WidthFixed, 96.0f);
                ImGui::TableSetupColumn("Info",   ImGuiTableColumnFlags_WidthStretch, 1.2f);
                ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 72.0f);
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < depthInfo.candidates.size(); ++i)
                {
                    const DepthCandidateInfo& c = depthInfo.candidates[i];
                    const bool isActive  = (c.imageView == depthInfo.active.imageView);
                    const bool thisPinned = isPinned && (c.imageView == depthInfo.pinnedView);

                    ImGui::TableNextRow();
                    ImGui::PushID(static_cast<int>(i));
                    if (isActive)
                        ImGui::PushStyleColor(ImGuiCol_Text, UI::Success());
                    else if (thisPinned)
                        ImGui::PushStyleColor(ImGuiCol_Text, UI::Warning());

                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%zu", i);

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", depthFormatName(c.format));

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%ux%u", c.extent.width, c.extent.height);

                    ImGui::TableSetColumnIndex(3);
                    {
                        std::string info;
                        info += sampleName(c.samples);
                        if (c.transient)
                            info += "  transient";
                        if (c.hasPresentableSnapshotTarget)
                            info += "  [swapchain]";
                        if (isActive)
                            info += "  ACTIVE";
                        else if (thisPinned)
                            info += "  PINNED";
                        ImGui::TextUnformatted(info.c_str());
                    }

                    ImGui::TableSetColumnIndex(4);
                    if (thisPinned)
                    {
                        if (ImGui::Button("Unpin", ImVec2(-FLT_MIN, 0)))
                            depthPinPendingClear = true;
                    }
                    else
                    {
                        if (ImGui::Button("Pin", ImVec2(-FLT_MIN, 0)))
                            depthPinPendingView = c.imageView;
                    }

                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::Text("View:      0x%llx", reinterpret_cast<unsigned long long>(c.imageView));
                        ImGui::Text("Layout:    %s", layoutName(c.observedLayout));
                        ImGui::Text("Draws:     %u", c.drawCount);
                        ImGui::Text("Transient: %s", c.transient ? "yes" : "no");
                        ImGui::EndTooltip();
                    }

                    if (isActive || thisPinned)
                        ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::M3CardEnd();
        }
        else
        {
            ImGui::TextDisabled("No depth views tracked yet. Render a frame with depth.");
        }

        // --- Depth decoding ---
        ImGui::Spacing();
        ImGui::M3CardBegin("adv_decode", "Depth resolving", Icon::ContrastUtf8);

        const char* depthModeNames[] = {
            "Luminance/Red (standard)",
            "Alpha (alpha-encoded)",
            "Packed RGB",
            "Logarithmic",
            "View-space Z",
            "NDC (Normalized Device Coords)",
            "Reversed-Z"
        };
        int dsc = settingsManager.getDepthSourceChannel();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Encoding");
        ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##depthSourceChannelCombo", &dsc, depthModeNames, IM_ARRAYSIZE(depthModeNames)))
        {
            settingsManager.setDepthSourceChannel(dsc);
            markSettingsDirty();
        }
        if (ImGui::IsItemHovered())
        {
            const char* tooltips[] = {
                "Read depth from R channel (luminance). Standard Vulkan depth format.",
                "Read depth from Alpha channel. Used by some custom renderers.",
                "Decode depth from packed RGB channels (e.g., RGB24/RGB32 encoding).",
                "Linearize logarithmic depth encoding (exp() linearization).",
                "Convert raw view-space Z to [0,1] range.",
                "Handle NDC depth values (post-projection coordinates).",
                "Reversed-Z encoding for improved depth precision."
            };
            ImGui::SetTooltip("%s", tooltips[dsc]);
        }

        bool depthInvert = settingsManager.getDepthInvert();
        if (ImGui::Checkbox("Invert depth values", &depthInvert))
        {
            settingsManager.setDepthInvert(depthInvert);
            markSettingsDirty();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("Invert depth values: depth = 1.0 - depth");
            ImGui::Text("Flips the near/far plane interpretation.");
            ImGui::TextColored(UI::Success(), "Can be toggled at runtime.");
            ImGui::EndTooltip();
        }
        ImGui::TextDisabled("Current: %s%s", depthModeNames[dsc], depthInvert ? ", INVERTED" : "");
        ImGui::M3CardEnd();

        // --- MSAA resolve ---
        ImGui::Spacing();
        ImGui::M3CardBegin("adv_resolve", "MSAA Resolve", Icon::BlurOnUtf8);
        const bool avgSupported = (depthInfo.supportedResolveModes & VK_RESOLVE_MODE_AVERAGE_BIT) != 0;
        int modePref = settingsManager.getDepthResolveMode();

        if (ImGui::RadioButton("Auto##resolveauto", modePref == 0))
        { settingsManager.setDepthResolveMode(0); markSettingsDirty(); }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Prefers average when the device supports it.");
        ImGui::SameLine();
        if (ImGui::RadioButton("Sample Zero##resolvezero", modePref == 1))
        { settingsManager.setDepthResolveMode(1); markSettingsDirty(); }
        ImGui::SameLine();
        ImGui::BeginDisabled(!avgSupported);
        if (ImGui::RadioButton("Average##resolveavg", modePref == 2))
        { settingsManager.setDepthResolveMode(2); markSettingsDirty(); }
        ImGui::EndDisabled();
        if (!avgSupported)
            ImGui::TextDisabled("Average mode not supported by this device.");
        ImGui::M3CardEnd();

        // --- Capture method ---
        ImGui::Spacing();
        ImGui::M3CardBegin("adv_capture", "Depth capture method", Icon::BoltUtf8);
        int dcm = settingsManager.getDepthCaptureMethod();
        if (ImGui::RadioButton("Off (legacy resolve only)##dcm0", dcm == 0))
        { settingsManager.setDepthCaptureMethod(0); markSettingsDirty(); }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Use the legacy per-swapchain resolve path only.\nNo persistent depth storage.");

        ImGui::Spacing();
        if (ImGui::RadioButton("Option A: RenderPass End##dcm1", dcm == 1))
        { settingsManager.setDepthCaptureMethod(1); markSettingsDirty(); }
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("Blit depth to a persistent storage image at each CmdEndRenderPass.");
            ImGui::Text("Low overhead (~1-2ms/frame). Recommended for most games.");
            ImGui::TextColored(UI::Warning(), "Auto-switches to Option B after 30s if no main-res captures.");
            ImGui::EndTooltip();
        }
        if (ImGui::RadioButton("Option B: QueueSubmit##dcm2", dcm == 2))
        { settingsManager.setDepthCaptureMethod(2); markSettingsDirty(); }
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("Intercept QueueSubmit to inject depth blit into the command stream.");
            ImGui::Text("More robust, slightly higher overhead.");
            ImGui::EndTooltip();
        }

        ImGui::Spacing();
        bool transientWorkaround = settingsManager.getDepthTransientWorkaround();
        if (ImGui::Checkbox("Transient attachment workaround", &transientWorkaround))
        {
            settingsManager.setDepthTransientWorkaround(transientWorkaround);
            markSettingsDirty();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("Forces STORE on depth attachments and tracks transient depth images.");
            ImGui::Text("Enable for games that render MSAA depth with transient attachments.");
            ImGui::EndTooltip();
        }
        ImGui::M3CardEnd();

        // --- Footer ---
        ImGui::Spacing();
        const std::string redetectLabel = std::string(Icon::RefreshUtf8) + "  Force reload depth buffers";
        if (ImGui::Button(redetectLabel.c_str()))
        {
            std::lock_guard<std::mutex> l(globalLock);
            logicalDevice->activeDepthState    = DepthState{};
            logicalDevice->bestDepthCandidate  = LogicalDevice::DepthCandidateTrackingState{};
            logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
            depthPinChanged = true;
            Logger::info("depth re-detect requested from Advanced UI");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Clear active depth buffer, best candidate, and any pin.\nThe layer will re-evaluate all render passes next frame.");

        // --- Deferred save ---
        flushSettingsSaveIfNeeded();

        ImGui::EndChild();
    }

} // namespace VKIntox
