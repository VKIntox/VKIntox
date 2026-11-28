#include "overlay/shell/imgui_overlay.hh"
#include "config/settings_manager.hh"
#include "layer/internal.hh"
#include "util/logger.hh"
#include "overlay/theme/ui_theme.hh"
#include "overlay/theme/ui_icons.hh"
#include "overlay/shell/ui_menu.hh"

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

        static double g_depthRefreshStarted = -1.0;
        static float g_depthRefreshReturnY = 0.0f;
        static bool g_depthRefreshScrollSaved = false;

        bool depthRefreshActive()
        {
            return g_depthRefreshStarted >= 0.0 && ImGui::GetTime() - g_depthRefreshStarted < 0.5;
        }

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
            const float radius = ImGuiM3Radius(ImGuiM3Shape_Large);
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
                : isPinned ? ImGuiM3ColorU32(ImGuiM3Role_OnPrimaryContainer)
                           : ImGuiM3ColorU32(ImGuiM3Role_OnSurface);
            const ImU32 supportCol = isActive ? ImGuiM3ColorU32(ImGuiM3Role_OnSecondaryContainer)
                : isPinned ? ImGuiM3ColorU32(ImGuiM3Role_OnPrimaryContainer)
                           : ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant);

            const float disc = 32.0f * d;
            const ImVec2 discCenter(mn.x + 14.0f * d + disc * 0.5f, mn.y + rowH * 0.5f);
            dl->AddCircleFilled(discCenter, disc * 0.5f,
                                isActive ? ImGuiM3ColorU32(ImGuiM3Role_Primary)
                                         : ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainerHighest), 32);
            const std::string indexText = std::to_string(index);
            const float indexPx = ImGuiM3GetTypeStyle(ImGuiM3Type_LabelSmall).size * d;
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

            const float btnH = m.button_height_default * d;
            const float btnW = 72.0f * d;
            const float rightX = mx.x - 12.0f * d;
            const char* badge = isPinned ? "Pinned" : "Active";
            const float badgeW = isActive || isPinned ? ImGui::CalcTextSize(badge).x + 24.0f * d : 0.0f;
            const float badgeX = rightX - btnW - 12.0f * d - badgeW;
            const float textRight = badgeW > 0.0f ? badgeX - 12.0f * d : rightX - btnW - 12.0f * d;
            const float textX = mn.x + 14.0f * d + disc + 12.0f * d;
            const float textW = ImMax(1.0f, textRight - textX);
            const float titlePx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyLarge).size * d;
            const float detailPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodySmall).size * d;
            ImGui::PushFont(brand, titlePx);
            const char* shownTitle = UI::TruncateLabel(sizeText.c_str(), textW);
            const ImVec2 titleSize = ImGui::CalcTextSize(shownTitle);
            ImGui::PopFont();
            ImGui::PushFont(bodyBold, detailPx);
            const char* shownDetail = UI::TruncateLabel(detail.c_str(), textW);
            const ImVec2 detailSize = ImGui::CalcTextSize(shownDetail);
            ImGui::PopFont();
            const float textBlockH = titleSize.y + detailSize.y + 2.0f * d;
            const float textY = mn.y + (rowH - textBlockH) * 0.5f;
            dl->AddText(brand, titlePx, ImVec2(textX, textY), titleCol, shownTitle);
            dl->AddText(bodyBold, detailPx, ImVec2(textX, textY + titleSize.y + 2.0f * d),
                        supportCol, shownDetail);

            if (isActive || isPinned)
            {
                ImGui::SetCursorScreenPos(ImVec2(badgeX, mn.y + (rowH - m.chip_height * d) * 0.5f));
                ImGui::M3StatusChip(badge, isPinned ? ImGuiM3Role_Primary
                                                    : ImGuiM3Role_PrimaryContainer);
            }

            ImGui::SetCursorScreenPos(ImVec2(rightX - btnW, mn.y + (rowH - btnH) * 0.5f));
            ImGui::SetNextItemAllowOverlap();
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

        if (UI::BreadcrumbBar("Settings", Icon::ChevronLeftUtf8))
        {
            openSubView = SubView_None;
            return;
        }
        ImGui::Spacing();
        bool depthCapture = settingsManager.getDepthCapture();
        {
            const float heroSize = 80.0f * d;
            const float gap = 16.0f * d;
            const float switchW = m.switch_track_width * d;
            const float switchH = m.list_item_height_1 * d;
            const float titlePx = ImGuiM3GetTypeStyle(ImGuiM3Type_TitleLarge).size * d;
            const float supportPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodySmall).size * d;
            ImFont* titleFont = ImGuiM3FontBold();
            ImFont* supportFont = ImGuiM3FontRegular();
            const char* support = "Choose how the layer finds, pins and decodes depth for effects.";
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float rowW = ImMax(ImGui::GetContentRegionAvail().x, heroSize + switchW + gap);
            const float textW = ImMax(1.0f, rowW - heroSize - gap - switchW - 20.0f * d);
            const ImVec2 titleSize = titleFont->CalcTextSizeA(titlePx, textW, 0.0f, "Depth buffer");
            const ImVec2 supportSize = supportFont->CalcTextSizeA(supportPx, textW, 0.0f, support);
            const float textH = titleSize.y + 4.0f * d + supportSize.y;
            const float rowH = ImMax(heroSize, textH);
            ImGui::Dummy(ImVec2(rowW, rowH));
            const ImRect shapeRect(pos, ImVec2(pos.x + heroSize, pos.y + heroSize));
            const ImGuiID shapeId = ImGui::GetID("##depth_capture_shape");
            ImGuiStorage* storage = ImGui::GetStateStorage();
            const float target = depthCapture ? 1.0f : 0.0f;
            float progress = storage->GetFloat(shapeId, target);
            const float duration = target > progress ? 0.35f : 0.22f;
            progress += ImClamp(target - progress, -ImGui::GetIO().DeltaTime / duration,
                                ImGui::GetIO().DeltaTime / duration);
            storage->SetFloat(shapeId, progress);
            const float morph = target > 0.5f ? 1.0f - ImPow(1.0f - progress, 3.0f) : progress;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImGuiM3DrawShapeMorph(dl, shapeRect, ImGuiM3MorphShape_Circle, ImGuiM3MorphShape_Cookie,
                                  morph, ImGuiM3ColorU32(ImGuiM3Role_PrimaryContainer));
            ImGuiM3DrawIcon(dl, Icon::LayersUtf8, shapeRect, heroSize * 0.46f,
                            ImGuiM3ColorU32(ImGuiM3Role_OnPrimaryContainer));
            const float textX = pos.x + heroSize + gap;
            const float textY = pos.y + (rowH - textH) * 0.5f;
            dl->AddText(titleFont, titlePx, ImVec2(textX, textY),
                        ImGuiM3ColorU32(ImGuiM3Role_OnSurface), "Depth buffer");
            dl->AddText(supportFont, supportPx, ImVec2(textX, textY + titleSize.y + 4.0f * d),
                        ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant), support);
            ImGui::SetCursorScreenPos(ImVec2(pos.x + rowW - switchW, pos.y + (rowH - switchH) * 0.5f));
            if (ImGui::M3SwitchScaled("", "##depth_capture_switch", &depthCapture, 1.0f))
            {
                settingsManager.setDepthCapture(depthCapture);
                markSettingsDirty();
                Logger::info(std::string("depth capture ") + (depthCapture ? "enabled" : "disabled") +
                             " (saved; takes effect on restart)");
                pushToastOnce(LogLevel::Info, "Depth buffer toggled. Applies on next launch.");
            }
            ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + rowH));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
        }
        ImGui::Spacing();

        auto drawSwitchRow = [&](const char* id, const char* label, const char* support, bool* value) {
            const float rowH = 72.0f * d;
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            const float rowW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
            const float switchW = m.switch_track_width * d;
            const float switchH = m.list_item_height_1 * d;
            const float leftW = ImMax(1.0f, rowW - switchW - 24.0f * d);
            const bool rowClicked = ImGui::InvisibleButton(id, ImVec2(leftW, rowH));
            if (rowClicked)
            {
                *value = !*value;
                ImGui::MarkItemEdited(ImGui::GetItemID());
            }
            const bool hovered = ImGui::IsItemHovered();
            const ImRect rowRect(rowMin, ImVec2(rowMin.x + rowW, rowMin.y + rowH));
            const float radius = ImGuiM3Radius(ImGuiM3Shape_Large);
            if (hovered)
                ImGuiM3DrawStateLayer(ImGui::GetWindowDrawList(), rowRect,
                                      ImGuiM3ShapeRounding{ radius, radius, radius, radius },
                                      ImGuiM3Role_OnSurface, ImGuiM3State_Hovered);
            const float textRight = rowRect.Max.x - switchW - 16.0f * d;
            const float labelPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyLarge).size * d;
            const float supportPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodySmall).size * d;
            ImFont* labelFont = ImGuiM3FontBold();
            ImFont* supportFont = ImGuiM3FontRegular();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float textLeft = rowMin.x + 4.0f * d;
            ImGui::PushFont(labelFont, labelPx);
            const char* shownLabel = UI::TruncateLabel(label, ImMax(1.0f, textRight - textLeft));
            const ImVec2 labelSize = ImGui::CalcTextSize(shownLabel);
            ImGui::PopFont();
            ImGui::PushFont(supportFont, supportPx);
            const char* shownSupport = UI::TruncateLabel(support, ImMax(1.0f, textRight - textLeft));
            const ImVec2 supportSize = support && support[0] ? ImGui::CalcTextSize(shownSupport) : ImVec2(0.0f, 0.0f);
            ImGui::PopFont();
            const float textH = labelSize.y + supportSize.y + (supportSize.y > 0.0f ? 2.0f * d : 0.0f);
            const float textY = rowMin.y + (rowH - textH) * 0.5f;
            dl->AddText(labelFont, labelPx, ImVec2(textLeft, textY),
                        ImGuiM3ColorU32(ImGuiM3Role_OnSurface), shownLabel);
            if (supportSize.y > 0.0f)
                dl->AddText(supportFont, supportPx, ImVec2(textLeft, textY + labelSize.y + 2.0f * d),
                            ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant), shownSupport);
            ImGui::SetCursorScreenPos(ImVec2(rowRect.Max.x - switchW - 12.0f * d,
                                             rowMin.y + (rowH - switchH) * 0.5f));
            const std::string switchId = std::string(id) + "_switch";
            const bool switchChanged = ImGui::M3SwitchWithID("", switchId.c_str(), value);
            if (support && support[0] && (hovered || ImGui::IsItemHovered()))
                ImGui::SetTooltip("%s", support);
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowRect.Max.y));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            return rowClicked || switchChanged;
        };

        auto drawSegmentRow = [&](const char* id, const char* label, const char* const* labels,
                                   const char* const* hints, int count, int* selected, const char* subtitle = "") {
            const float rowH = 72.0f * d;
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            const float rowW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
            const float groupW = ImMax(150.0f * d, ImMin(270.0f * d, rowW * 0.58f));
            const float leftW = ImMax(1.0f, rowW - groupW - 20.0f * d);
            ImGui::InvisibleButton(id, ImVec2(leftW, rowH));
            const bool hovered = ImGui::IsItemHovered();
            const ImRect rowRect(rowMin, ImVec2(rowMin.x + rowW, rowMin.y + rowH));
            const float radius = ImGuiM3Radius(ImGuiM3Shape_Large);
            if (hovered)
                ImGuiM3DrawStateLayer(ImGui::GetWindowDrawList(), rowRect,
                                      ImGuiM3ShapeRounding{ radius, radius, radius, radius },
                                      ImGuiM3Role_OnSurface, ImGuiM3State_Hovered);
            const float groupH = m.button_height_default * d;
            const float labelPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyLarge).size * d;
            ImFont* labelFont = ImGuiM3FontBold();
            ImFont* subtitleFont = ImGuiM3FontRegular();
            const float subtitlePx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodySmall).size * d;
            const float textW = ImMax(1.0f, rowW - groupW - 28.0f * d);
            ImGui::PushFont(labelFont, labelPx);
            const char* shownLabel = UI::TruncateLabel(label, textW);
            const ImVec2 labelSize = ImGui::CalcTextSize(shownLabel);
            ImGui::PopFont();
            ImGui::PushFont(subtitleFont, subtitlePx);
            const ImVec2 subtitleSize = subtitle && subtitle[0]
                ? ImGui::CalcTextSize(subtitle) : ImVec2(0.0f, 0.0f);
            ImGui::PopFont();
            const float textH = labelSize.y + (subtitleSize.y > 0.0f ? 2.0f * d + subtitleSize.y : 0.0f);
            const float textY = rowMin.y + (rowH - textH) * 0.5f;
            ImGui::GetWindowDrawList()->AddText(
                labelFont, labelPx, ImVec2(rowMin.x + 4.0f * d, textY),
                ImGuiM3ColorU32(ImGuiM3Role_OnSurface), shownLabel);
            if (subtitleSize.y > 0.0f)
                ImGui::GetWindowDrawList()->AddText(
                    subtitleFont, subtitlePx, ImVec2(rowMin.x + 4.0f * d, textY + labelSize.y + 2.0f * d),
                    ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant), subtitle);
            int hoveredSegment = -1;
            ImGui::SetCursorScreenPos(ImVec2(rowRect.Max.x - groupW - 4.0f * d,
                                             rowMin.y + (rowH - groupH) * 0.5f));
            const std::string groupId = std::string(id) + "_group";
            const bool changed = ImGui::M3ConnectedButtonGroup(groupId.c_str(), labels, count, selected,
                                                               NULL, false, &hoveredSegment);
            if (hoveredSegment >= 0)
                ImGui::SetTooltip("%s", hints[hoveredSegment]);
            else if (hovered)
                ImGui::SetTooltip("Choose a mode. Hover a segment for details.");
            ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowRect.Max.y));
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            return changed;
        };

        ImGui::Spacing();
        {
            const char* modes[] = {"Auto", "Manual pin"};
            const char* hints[] = {
                "Auto promotes the buffer with the most draws, or a swapchain-linked one.",
                "Manual pin starts from the buffer matching the window size; pick another from the list."
            };
            int mode = depthInfo.depthIsPinned ? 1 : 0;
            std::string subtitle;
            if (depthInfo.active.imageView != VK_NULL_HANDLE)
            {
                const char* format = depthInfo.active.format == VK_FORMAT_R32_SFLOAT
                    ? "R32_SFLOAT" : depthFormatName(depthInfo.active.format);
                subtitle = std::to_string(depthInfo.active.extent.width) + "x" +
                           std::to_string(depthInfo.active.extent.height) + " " + format;
            }
            if (drawSegmentRow("##depth_mode", "Buffer selection", modes, hints, 2, &mode,
                               subtitle.c_str()))
            {
                if (mode == 0)
                {
                    if (depthInfo.depthIsPinned)
                        depthPinPendingClear = true;
                }
                else if (!depthInfo.candidates.empty())
                    depthPinPendingView = defaultPinCandidateForSwapchain(depthInfo);
                else
                    pushToast(LogLevel::Warn, "No depth buffers are tracked yet. Render a frame first.");
            }
        }

        ImGui::Dummy(ImVec2(0.0f, 2.0f * d));
        {
            const std::string trailing = std::to_string(depthInfo.candidates.size());
            if (UI::SubViewRow("Reported depth buffers", trailing.c_str(), Icon::GridViewUtf8))
                openSubView = SubView_DepthBuffers;
        }
        ImGui::Dummy(ImVec2(0.0f, 2.0f * d));

        {
            static const int kChannels[] = {0, 4};
            const char* labels[] = {"Luminance/Red", "View-space Z"};
            const char* hints[] = {
                "Read depth from the red / luminance channel. The standard Vulkan depth layout.",
                "Convert raw view-space Z into [0,1]. Use when the red channel reads flat or inverted."
            };
            int current = settingsManager.getDepthSourceChannel();
            int index = current == kChannels[1] ? 1 : 0;
            if (drawSegmentRow("##depth_channel", "Encoding", labels, hints, 2, &index))
            {
                settingsManager.setDepthSourceChannel(kChannels[index]);
                depthTransformPending = true;
                markSettingsDirty();
            }
        }

        {
            bool depthInvert = settingsManager.getDepthInvert();
            bool depthFlip = settingsManager.getDepthFlip();
            bool depthFlop = settingsManager.getDepthFlop();
            // The three transform switches use the same row geometry as capture.
            auto transformSwitch = [&](const char* id, const char* label, const char* support, bool* value) {
                return drawSwitchRow(id, label, support, value);
            };
            if (transformSwitch("##depth_invert_value", "Invert depth",
                                "Flip the near/far interpretation: depth = 1.0 - depth.", &depthInvert))
            {
                settingsManager.setDepthInvert(depthInvert);
                depthTransformPending = true;
                markSettingsDirty();
            }
            if (transformSwitch("##depth_flip_value", "Flip vertically",
                                "Mirror the depth image vertically (uv.y -> 1 - uv.y).", &depthFlip))
            {
                settingsManager.setDepthFlip(depthFlip);
                depthTransformPending = true;
                markSettingsDirty();
            }
            if (transformSwitch("##depth_flop_value", "Mirror horizontally",
                                "Mirror the depth image horizontally (uv.x -> 1 - uv.x).", &depthFlop))
            {
                settingsManager.setDepthFlop(depthFlop);
                depthTransformPending = true;
                markSettingsDirty();
            }
        }

        {
            const char* labels[] = {"Safe", "Fast"};
            const char* hints[] = {
                "Idle the GPU before rebuilding.",
                "Rebuild using our own submit fences without idling. Not recommended."
            };
            int index = settingsManager.getDepthRebuildMode() == 0 ? 0 : 1;
            if (drawSegmentRow("##depth_rebuild_mode", "Rebuild", labels, hints, 2, &index))
            {
                settingsManager.setDepthRebuildMode(index);
                markSettingsDirty();
            }
        }

        ImGui::Spacing();
        const bool refreshing = depthRefreshActive();
        bool refreshStartedThisFrame = false;
        const std::string redetectLabel = std::string(Icon::RefreshUtf8) + "  Refresh depth buffers";
        ImGui::BeginDisabled(refreshing || !logicalDevice);
        if (ImGui::M3Button(redetectLabel.c_str(), ImGuiM3Button_Tonal))
        {
            std::lock_guard<std::mutex> l(globalLock);
            logicalDevice->activeDepthState = DepthState{};
            logicalDevice->bestDepthCandidate = LogicalDevice::DepthCandidateTrackingState{};
            logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
            depthPinChanged = true;
            g_depthRefreshStarted = ImGui::GetTime();
            refreshStartedThisFrame = true;
            if (!g_depthRefreshScrollSaved)
            {
                g_depthRefreshReturnY = ImGui::GetScrollY();
                g_depthRefreshScrollSaved = true;
            }
            Logger::info("depth re-detect requested from Advanced UI");
            pushToast(LogLevel::Info, "Re-detecting depth buffers.");
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", !logicalDevice ? "The Vulkan device is not ready."
                : "Clear the active buffer, the best candidate and any pin. The layer re-evaluates every render pass next frame.");
        }
        if (refreshing || refreshStartedThisFrame)
        {
            ImGui::M3LinearProgress(0.5f, ImVec2(-1, 0), true);
            UI::TextSupport("Re-detecting depth buffers. Render a frame if the list stays empty.");
        }
        if (refreshStartedThisFrame)
            ImGui::SetScrollY(ImGui::GetScrollMaxY());
        else if (!refreshing && g_depthRefreshScrollSaved)
        {
            ImGui::SetScrollY(g_depthRefreshReturnY);
            g_depthRefreshScrollSaved = false;
        }

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
        ImGui::M3SectionHeader("Reported depth buffers", Icon::LayersUtf8);
        UI::TextSupport(describeActiveDepth(depthInfo).c_str());
        ImGui::Spacing();
        {
            const char* modes[] = {"Auto", "Manual pin"};
            const char* hints[] = {
                "Auto promotes the buffer with the most draws, or a swapchain-linked one.",
                "Manual pin starts from the buffer matching the window size; pick another from the list."
            };
            int mode = depthInfo.depthIsPinned ? 1 : 0;
            int hoveredSegment = -1;
            if (ImGui::M3ConnectedButtonGroup("##depth_page_mode", modes, 2, &mode,
                                              NULL, false, &hoveredSegment))
            {
                if (mode == 0)
                {
                    if (depthInfo.depthIsPinned)
                        depthPinPendingClear = true;
                }
                else if (!depthInfo.candidates.empty())
                    depthPinPendingView = defaultPinCandidateForSwapchain(depthInfo);
                else
                    pushToast(LogLevel::Warn, "No depth buffers are tracked yet. Render a frame first.");
            }
            if (hoveredSegment >= 0)
                ImGui::SetTooltip("%s", hints[hoveredSegment]);
        }

        ImGui::Spacing();
        if (!logicalDevice || depthInfo.candidates.empty())
        {
            if (depthRefreshActive() && logicalDevice)
            {
                ImGui::M3LinearProgress(0.5f, ImVec2(-1, 0), true);
                UI::TextSupport("Re-detecting depth buffers. Render a frame to populate the list.");
            }
            else if (!logicalDevice)
            {
                ImGui::PushFont(ImGuiM3FontBold(),
                                ImGuiM3GetTypeStyle(ImGuiM3Type_TitleMedium).size * d);
                ImGui::TextUnformatted("Vulkan device not ready");
                ImGui::PopFont();
                UI::TextSupport("Depth views appear after the layer creates its render device.");
            }
            else if (!settingsManager.getDepthCapture())
            {
                ImGui::PushFont(ImGuiM3FontBold(),
                                ImGuiM3GetTypeStyle(ImGuiM3Type_TitleMedium).size * d);
                ImGui::TextUnformatted("Depth capture is off");
                ImGui::PopFont();
                UI::TextSupport("Enable Capture depth buffer in Settings, then restart the game.");
            }
            else
            {
                ImGui::PushFont(ImGuiM3FontBold(),
                                ImGuiM3GetTypeStyle(ImGuiM3Type_TitleMedium).size * d);
                ImGui::TextUnformatted("No depth views tracked yet");
                ImGui::PopFont();
                UI::TextSupport("Render a frame with depth, then refresh this list.");
            }
            flushSettingsSaveIfNeeded();
            return;
        }

        const float rowH = 72.0f * d;
        const float rowGap = 6.0f * d;
        ImGui::BeginChild("##depth_page_list", ImVec2(0, 0), false);
        for (size_t i = 0; i < depthInfo.candidates.size(); ++i)
        {
            const DepthCandidateInfo& c = depthInfo.candidates[i];
            const bool isActive = c.imageView == depthInfo.active.imageView;
            const bool isPinned = depthInfo.depthIsPinned && c.imageView == depthInfo.pinnedView;
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
