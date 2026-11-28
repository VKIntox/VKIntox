#include "overlay/shell/imgui_overlay.hh"
#include "effects/runtime/effect_registry.hh"
#include "config/settings_manager.hh"
#include "effects/compiler/reshade_parser.hh"
#include "config/config_serializer.hh"
#include "overlay/shell/ui_dialog.hh"
#include "overlay/shell/ui_menu.hh"
#include "util/util.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <set>
#include <string>
#include <vector>

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imgui_internal.h"
#include "vendor/imgui/imgui_m3.h"
#include "overlay/theme/ui_theme.hh"
#include "overlay/theme/ui_icons.hh"
#include "overlay/theme/effect_icons.hh"

namespace VKIntox
{
    // ---- Effect search -------------------------------------------------------
    //
    // The list routinely holds 300+ ReShade shaders, so the view is built around
    // a ranked, instantly-filtered search instead of a long scroll. Scoring is
    // cheap and deterministic: exact prefix beats word-prefix beats substring,
    // with a subsequence fallback so "sm" still finds "SMAA" and "smaa".

    static std::string toLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    static bool isSubsequence(const std::string& needle, const std::string& haystack)
    {
        size_t j = 0;
        for (size_t i = 0; i < haystack.size() && j < needle.size(); i++)
            if (haystack[i] == needle[j])
                j++;
        return j == needle.size();
    }

    // Lower score = better. Returns -1 when there is no match.
    static int matchScore(const std::string& name, const std::string& query)
    {
        if (query.empty())
            return 0;
        const std::string n = toLower(name);
        const std::string q = toLower(query);
        if (n.rfind(q, 0) == 0)
            return 0;   // prefix
        // word-prefix: the query starts a word inside the name
        for (size_t i = 1; i < n.size(); i++)
            if ((n[i - 1] == ' ' || n[i - 1] == '_' || n[i - 1] == '-') && n.compare(i, q.size(), q) == 0)
                return 1;
        if (n.find(q) != std::string::npos)
            return 2;   // substring
        if (isSubsequence(q, n))
            return 3;   // fuzzy
        return -1;
    }

    // The first directory under the shader root names the effect package. Nested
    // installs (Shaders/Package/Sub/Effect.fx) still report the top-level package,
    // and a shader sitting directly in Shaders/ reports none.
    static std::string effectPackageForPath(const std::string& path)
    {
        std::vector<std::string> parts;
        std::string current;
        for (char c : path)
        {
            if (c == '/' || c == '\\')
            {
                if (!current.empty()) parts.push_back(current);
                current.clear();
            }
            else
            {
                current.push_back(c);
            }
        }
        if (!current.empty()) parts.push_back(current);

        size_t shaders = parts.size();
        for (size_t i = 0; i < parts.size(); i++)
            if (parts[i] == "Shaders" || parts[i] == "Textures")
                shaders = i;
        return (shaders + 2 < parts.size()) ? parts[shaders + 1] : std::string();
    }

    void ImGuiOverlay::renderAddEffectsView()
    {
        if (!effectRegistry)
            return;

        std::vector<std::string> selectedEffects = effectRegistry->getSelectedEffects();

        const ImGuiM3Metrics& m = ImGuiM3GetMetrics();
        const float d = m.density;

        // ESC clears the search (and, when already empty, leaves the view).
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && addEffectsSearch[0] != '\0')
            addEffectsSearch[0] = '\0';

        // Seamless typing: capture printable characters when no widget is focused.
        if (!ImGui::IsAnyItemActive())
        {
            ImGuiIO& io = ImGui::GetIO();
            if (io.InputQueueCharacters.Size > 0)
            {
                for (int i = 0; i < io.InputQueueCharacters.Size; i++)
                {
                    ImWchar c = io.InputQueueCharacters[i];
                    if (c >= 32 && c < 127)
                    {
                        size_t len = strlen(addEffectsSearch);
                        if (len < sizeof(addEffectsSearch) - 1)
                        {
                            addEffectsSearch[len] = static_cast<char>(c);
                            addEffectsSearch[len + 1] = '\0';
                        }
                    }
                }
                io.InputQueueCharacters.clear();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && addEffectsSearch[0] != '\0')
            {
                size_t len = strlen(addEffectsSearch);
                if (len > 0)
                    addEffectsSearch[len - 1] = '\0';
            }
        }

        const size_t maxEffectsLimit = static_cast<size_t>(settingsManager.getMaxEffects());
        const size_t currentCount = selectedEffects.size();
        auto removedPresentCount = [&]() {
            size_t count = 0;
            for (const auto& removed : addEffectsRemoved)
                if (std::find(selectedEffects.begin(), selectedEffects.end(), removed) != selectedEffects.end())
                    count++;
            return count;
        };
        auto projectedCount = [&]() {
            return currentCount - removedPresentCount() + pendingAddEffects.size();
        };
        const bool atLimit = projectedCount() >= maxEffectsLimit;

        auto isNameUsed = [&](const std::string& name) {
            if (std::find(selectedEffects.begin(), selectedEffects.end(), name) != selectedEffects.end())
                return true;
            for (const auto& p : pendingAddEffects)
                if (p.first == name)
                    return true;
            return false;
        };
        auto getNextInstanceName = [&](const std::string& effectType) -> std::string {
            if (!isNameUsed(effectType))
                return effectType;
            for (int n = 2; n < static_cast<int>(maxEffectsLimit) + 2; n++)
            {
                std::string candidate = effectType + "." + std::to_string(n);
                if (!isNameUsed(candidate))
                    return candidate;
            }
            return {};
        };

        // Session-scoped recents, so frequently used shaders are one click away.
        static std::vector<std::string> recent;
        auto pushRecent = [&](const std::string& type) {
            auto it = std::find(recent.begin(), recent.end(), type);
            if (it != recent.end())
                recent.erase(it);
            recent.insert(recent.begin(), type);
            if (recent.size() > 8)
                recent.resize(8);
        };
        auto alreadyInConfig = [&](const std::string& type) {
            return std::find(selectedEffects.begin(), selectedEffects.end(), type) != selectedEffects.end();
        };
        auto pendingCountFor = [&](const std::string& type) {
            return static_cast<int>(std::count_if(pendingAddEffects.begin(), pendingAddEffects.end(),
                [&](const auto& p) { return p.second == type; }));
        };
        auto extraCopiesFor = [&](const std::string& type) {
            auto it = addEffectsDuplicateCount.find(type);
            return (it == addEffectsDuplicateCount.end()) ? 0 : std::max(0, it->second);
        };
        // the displayed count includes the original active effect, or the one
        // base copy queued for an effect that is not in the preset yet
        auto copiesFor = [&](const std::string& type) {
            return extraCopiesFor(type) + 1;
        };
        auto groupActive = [&](const std::string& type) {
            return pendingCountFor(type) > 0 ||
                   (alreadyInConfig(type) && addEffectsRemoved.count(type) == 0);
        };
        auto removeQueued = [&](const std::string& type) {
            pendingAddEffects.erase(std::remove_if(pendingAddEffects.begin(), pendingAddEffects.end(),
                                                   [&](const auto& p) { return p.second == type; }),
                                    pendingAddEffects.end());
        };
        auto queueCopies = [&](const std::string& type, int totalCopies) {
            removeQueued(type);
            const int retained = alreadyInConfig(type) && addEffectsRemoved.count(type) == 0 ? 1 : 0;
            const int needed = std::max(0, totalCopies - retained);
            for (int i = 0; i < needed && projectedCount() < maxEffectsLimit; i++)
            {
                const std::string instance = getNextInstanceName(type);
                if (instance.empty())
                    break;
                pendingAddEffects.push_back({instance, type});
            }
            const int actualCopies = retained + pendingCountFor(type);
            addEffectsDuplicateCount[type] = std::max(0, actualCopies - 1);
            pushRecent(type);
        };
        auto queueEffect = [&](const std::string& effectType) {
            if (projectedCount() >= maxEffectsLimit || groupActive(effectType))
                return;
            addEffectsRemoved.erase(effectType);
            addEffectsDuplicateCount[effectType] = 0;
            queueCopies(effectType, 1);
        };
        auto toggleQueued = [&](const std::string& type, bool on) {
            removeQueued(type);
            if (!on)
            {
                if (alreadyInConfig(type))
                    addEffectsRemoved.insert(type);
                return;
            }
            addEffectsRemoved.erase(type);
            queueCopies(type, copiesFor(type));
        };

        // ---- Build the working set ------------------------------------------
        // Rebuilding and re-sorting 300+ shaders every frame is wasted when the
        // inputs rarely move, so cache the result until state, search or package
        // tab actually changes.
        static std::string filterPackage;  // empty = All
        if (overlayStateVersion != addEffectsCacheVersion
            || std::string(addEffectsSearch) != addEffectsCacheSearch
            || filterPackage != addEffectsCacheFilter)
        {
            addEffectsEntries.clear();

            std::vector<std::string> sortedCurrent = state.currentConfigEffects;
            std::vector<std::string> sortedDefault = state.defaultConfigEffects;
            std::sort(sortedCurrent.begin(), sortedCurrent.end());
            std::sort(sortedDefault.begin(), sortedDefault.end());

            auto addEntry = [&](const std::string& type, const std::string& path, int group) {
                const int score = matchScore(type, addEffectsSearch);
                if (score >= 0)
                    addEffectsEntries.push_back({type, path, effectPackageForPath(path), group, score});
            };
            for (const auto& et : sortedCurrent)
            {
                auto it = state.effectPaths.find(et);
                addEntry(et, (it != state.effectPaths.end()) ? it->second : "", 0);
            }
            for (const auto& et : sortedDefault)
            {
                if (std::find(sortedCurrent.begin(), sortedCurrent.end(), et) != sortedCurrent.end())
                    continue;
                auto it = state.effectPaths.find(et);
                addEntry(et, (it != state.effectPaths.end()) ? it->second : "", 1);
            }

            // the tabs are the installed packages, not the working set: a package
            // whose effects are all already added has to stay reachable so its
            // duplicates can still be queued
            {
                std::set<std::string> packages;
                for (const auto& entry : state.effectPaths)
                {
                    const std::string package = effectPackageForPath(entry.second);
                    if (!package.empty())
                        packages.insert(package);
                }
                addEffectsPackages.assign(packages.begin(), packages.end());
                // VKIntox's own shaders lead the tab row; the rest stay alphabetical
                std::sort(addEffectsPackages.begin(), addEffectsPackages.end(),
                          [](const std::string& a, const std::string& b) {
                              if ((a == "VKIntox") != (b == "VKIntox"))
                                  return a == "VKIntox";
                              return a < b;
                          });
            }

            // a tab can vanish when the install changes; fall back to All
            if (!filterPackage.empty() &&
                std::find(addEffectsPackages.begin(), addEffectsPackages.end(), filterPackage) == addEffectsPackages.end())
                filterPackage.clear();

            if (!filterPackage.empty())
                addEffectsEntries.erase(std::remove_if(addEffectsEntries.begin(), addEffectsEntries.end(),
                                                       [&](const AddEffectsEntry& e) { return e.package != filterPackage; }),
                                        addEffectsEntries.end());

            // group results by their effect package, keeping search relevance first
            std::stable_sort(addEffectsEntries.begin(), addEffectsEntries.end(), [](const AddEffectsEntry& a, const AddEffectsEntry& b) {
                if (a.score != b.score)
                    return a.score < b.score;
                if (a.package != b.package)
                    return a.package < b.package;
                if (a.group != b.group)
                    return a.group < b.group;
                return a.type < b.type;
            });

            // the All tab, with no active search, shows a dimmed header per package
            addEffectsRows.clear();
            const bool groupHeads = filterPackage.empty() && addEffectsSearch[0] == '\0';
            std::string currentPackage;
            for (int i = 0; i < static_cast<int>(addEffectsEntries.size()); i++)
            {
                if (groupHeads && addEffectsEntries[i].package != currentPackage)
                {
                    currentPackage = addEffectsEntries[i].package;
                    addEffectsRows.push_back({currentPackage.empty() ? std::string("Other") : currentPackage, -1});
                }
                addEffectsRows.push_back({std::string(), i});
            }

            addEffectsCacheVersion = overlayStateVersion;
            addEffectsCacheSearch = addEffectsSearch;
            addEffectsCacheFilter = filterPackage;
        }
        const std::vector<AddEffectsEntry>& entries = addEffectsEntries;
        const std::vector<AddEffectsRow>& rows = addEffectsRows;

        const int rowCount = static_cast<int>(rows.size());
        const bool hasSearch = addEffectsSearch[0] != '\0';

        // Keyboard cursor. Indexes display rows, which may include package headers.
        static int highlight = 0;
        if (rowCount == 0)
            highlight = 0;
        else
            highlight = std::clamp(highlight, 0, rowCount - 1);

        if (rowCount > 0)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                highlight = std::min(highlight + 1, rowCount - 1);
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                highlight = std::max(highlight - 1, 0);
        }

        // ---- Crumb ----------------------------------------------------------
        if (UI::BreadcrumbBar("Effects", Icon::ChevronLeftUtf8))
        {
            pendingAddEffects.clear();
            addEffectsDuplicateCount.clear();
            addEffectsRemoved.clear();
            insertPosition = -1;
            inSelectionMode = false;
            addEffectsSearch[0] = '\0';
        }
        const bool portalPending = settingsManager.getUsePortalIntegration() && fileDialogPending();
        if (portalPending)
            UI::TextSupport("Finish the open picker in Effects to continue.");
        else if (atLimit)
            UI::TextSupport("Limit reached. Switch off or remove an effect to free a slot.");

        // ---- Search ---------------------------------------------------------
        // Focus the search box once the slide has settled. Focusing during the
        // transition made ImGui scroll to the item and dragged the nav bar in.
        if (addEffectsFocusSearch && !viewTransitionActive)
        {
            ImGui::SetKeyboardFocusHere();
            addEffectsFocusSearch = false;
        }
        {
            const float searchH = 48.0f * d;
            const float searchPadY = ImMax(0.0f, (searchH - ImGui::GetFontSize()) * 0.5f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(48.0f * d, searchPadY));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGuiM3Radius(ImGuiM3Shape_Full));
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##search", "Search effects...",
                                     addEffectsSearch, sizeof(addEffectsSearch),
                                     ImGuiInputTextFlags_AutoSelectAll);
            const ImVec2 afterSearch = ImGui::GetCursorScreenPos();
            const ImVec2 fmin = ImGui::GetItemRectMin();
            const ImVec2 fmax = ImGui::GetItemRectMax();
            if (ImGuiM3IconFont())
            {
                ImGuiM3DrawIcon(ImGui::GetWindowDrawList(), Icon::SearchUtf8,
                                ImRect(ImVec2(fmin.x + 12.0f * d, fmin.y),
                                       ImVec2(fmin.x + 40.0f * d, fmax.y)),
                                22.0f * d, ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant));
            }
            if (hasSearch)
            {
                const float clearSize = m.icon_button_size * d;
                ImGui::SetCursorScreenPos(ImVec2(fmax.x - clearSize - 8.0f * d,
                                                 fmin.y + (searchH - clearSize) * 0.5f));
                ImGui::PushID("search_clear");
                ImGui::SetNextItemAllowOverlap();
                if (ImGui::M3IconButton(Icon::CloseUtf8, "Clear search", ImGuiM3Button_Text))
                    addEffectsSearch[0] = '\0';
                ImGui::PopID();
            }
            ImGui::SetCursorScreenPos(afterSearch);
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            ImGui::PopStyleVar(2);
        }

        // Package tabs: All plus one per installed effect package, in a
        // horizontally scrolling row so a long package list stays usable.
        {
            ImGui::BeginChild("##packagetabs", ImVec2(0, 46.0f * d), false, ImGuiWindowFlags_HorizontalScrollbar);
            auto tab = [&](const char* label, const std::string& package) {
                bool selected = (filterPackage == package);
                ImGui::PushID(label);
                if (ImGui::M3Chip(label, &selected))
                    filterPackage = selected ? package : std::string();
                ImGui::PopID();
                ImGui::SameLine();
            };
            tab("All", std::string());
            for (const auto& package : addEffectsPackages)
                tab(package.c_str(), package);

            // scroll the tab row with the mouse. a vertical wheel drives the
            // horizontal scroll; when a device reports both axes, prefer the
            // horizontal one so the two never stack.
            if (ImGui::IsWindowHovered())
            {
                const ImGuiIO& io = ImGui::GetIO();
                const float wheel = io.MouseWheelH != 0.0f ? io.MouseWheelH : io.MouseWheel;
                if (wheel != 0.0f)
                    ImGui::SetScrollX(ImGui::GetScrollX() - wheel * 48.0f * d);
            }
            ImGui::EndChild();

            // outside the scrolling child: a second line inside it made the
            // horizontal scrollbar fight the (auto) vertical one for width.
            UI::PushSupport();
            ImGui::TextDisabled("%d result%s", static_cast<int>(entries.size()), entries.size() == 1 ? "" : "s");
            UI::PopSupport();
        }

        // Recents (only when browsing, not searching).
        if (!hasSearch && !recent.empty())
        {
            ImFont* recentFont = ImGuiM3FontMedium();
            if (recentFont)
                ImGui::PushFont(recentFont, ImGui::GetFontSize() * 0.85f);
            ImGui::TextDisabled("Recent");
            if (recentFont)
                ImGui::PopFont();
            bool firstRecent = true;
            for (const std::string& r : recent)
            {
                if (!firstRecent)
                    ImGui::SameLine();
                firstRecent = false;
                ImGui::PushID(r.c_str());
                if (ImGui::M3Button(r.c_str(), ImGuiM3Button_Outlined, ImVec2(0, 26.0f * d)))
                    queueEffect(r);
                ImGui::PopID();
            }
        }

        // ---- Results (virtualized) -----------------------------------------
        // Reserve the real footer height: the Cancel/Add row is a pair of M3
        // buttons, which are taller than the default frame, so measuring with
        // GetFrameHeightWithSpacing() left the bottom of the pills clipped.
        const float footerHeight = m.button_height_default * d + ImGui::GetStyle().ItemSpacing.y * 2.0f;
        const float rowH = 72.0f * d;
        const float trayChipH = m.chip_height * d;
        ImFont* iconFont = ImGuiM3IconFont();
        ImFont* medium = ImGuiM3FontMedium();

        // Switching package tabs shoves the list sideways and fades it in, so the
        // change of contents is legible instead of instantaneous.
        static std::string tabAnimShown = filterPackage;
        static double tabAnimStart = -1.0;
        float tabEase = 1.0f;
        {
            constexpr float kTabSeconds = 0.24f;
            const double now = ImGui::GetTime();
            if (tabAnimShown != filterPackage)
            {
                tabAnimShown = filterPackage;
                tabAnimStart = now;
            }
            if (tabAnimStart >= 0.0)
            {
                const float clock = static_cast<float>((now - tabAnimStart) / kTabSeconds);
                if (clock >= 1.0f)
                {
                    tabAnimStart = -1.0;
                }
                else
                {
                    tabEase = UI::CubicBezierEase(clock, 0.05f, 0.90f, 0.10f, 1.06f);
                }
            }
        }

        const ImVec2 resultsPos = ImGui::GetCursorScreenPos();
        const float resultsW = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
        int trayLines = 1;
        if (!pendingAddEffects.empty())
        {
            const float trayAvail = ImMax(1.0f, resultsW - 24.0f * d);
            float x = 0.0f;
            for (const auto& pending : pendingAddEffects)
            {
                const std::string label = pending.first + "  " + Icon::CloseUtf8;
                const float chipW = ImGui::M3ButtonWidth(label.c_str());
                if (x > 0.0f && x + chipW > trayAvail)
                {
                    trayLines++;
                    x = 0.0f;
                }
                x += chipW + ImGui::GetStyle().ItemSpacing.x;
            }
        }
        const float trayHeight = pendingAddEffects.empty() ? 48.0f * d
            : ImMin(112.0f * d, 16.0f * d + trayLines * (trayChipH + 4.0f * d));
        const float resultsH = ImMax(ImGui::GetContentRegionAvail().y, 1.0f);
        const float resultsSlide = (1.0f - tabEase) * 26.0f * d;
        ImGui::SetCursorScreenPos(ImVec2(resultsPos.x + resultsSlide, resultsPos.y));
        ImGui::PushClipRect(resultsPos, ImVec2(resultsPos.x + resultsW, resultsPos.y + resultsH), true);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, tabEase);
        ImGui::BeginChild("##results", ImVec2(0, -footerHeight - trayHeight), true,
                          ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f)
            ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseWheel * 32.0f * d);
        if (rowCount == 0)
        {
            const bool filtered = hasSearch || !filterPackage.empty();
            const bool hasInstalledPackages = !addEffectsPackages.empty();
            ImGui::Spacing();
            const ImVec2 emptyPos = ImGui::GetCursorScreenPos();
            const float emptyIcon = 40.0f * d;
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(emptyPos.x + emptyIcon * 0.5f, emptyPos.y + emptyIcon * 0.5f), emptyIcon * 0.5f,
                ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainerHighest), 40);
            if (iconFont)
            {
                ImGuiM3DrawIcon(ImGui::GetWindowDrawList(), Icon::SearchUtf8,
                                ImRect(emptyPos, ImVec2(emptyPos.x + emptyIcon, emptyPos.y + emptyIcon)),
                                emptyIcon * 0.56f,
                                ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant));
            }
            ImGui::SameLine(0.0f, 12.0f * d);
            ImGui::BeginGroup();
            ImGui::PushFont(ImGuiM3FontBold(),
                            ImGuiM3GetTypeStyle(ImGuiM3Type_TitleMedium).size * d);
            ImGui::TextUnformatted(filtered ? "No matching effects"
                : hasInstalledPackages ? "No effects in this working set" : "No effects installed");
            ImGui::PopFont();
            UI::TextSupport(filtered
                ? "Try another search or clear the package filter."
                : hasInstalledPackages
                    ? "The active and default configs do not list any effects yet."
                    : "Add shader directories in Shaders, then rescan to find effects.");
            ImGui::EndGroup();
            if (filtered)
            {
                ImGui::Spacing();
                if (ImGui::M3Button("Clear search and filters", ImGuiM3Button_Outlined))
                {
                    addEffectsSearch[0] = '\0';
                    filterPackage.clear();
                    highlight = 0;
                }
            }
        }
        else
        {
            ImGuiListClipper clipper;
            clipper.Begin(rowCount, rowH);
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
                {
                    const AddEffectsRow& row = rows[i];
                    if (!row.header.empty())
                    {
                        // dimmed, smaller package label; the row height supplies the gap
                        const ImVec2 headerPos = ImGui::GetCursorScreenPos();
                        ImGui::Dummy(ImVec2(1.0f, rowH));
                        ImDrawList* headerList = ImGui::GetWindowDrawList();
                        const float headerSize = ImGui::GetFontSize() * 0.85f;
                        headerList->AddText(medium ? medium : ImGui::GetFont(), headerSize,
                                            ImVec2(headerPos.x + 8.0f * d, headerPos.y + (rowH - headerSize) * 0.5f),
                                            ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant), row.header.c_str());
                        continue;
                    }

                    const AddEffectsEntry& e = entries[row.entry];
                    ImGui::PushID(i);
                    const ImVec2 pos = ImGui::GetCursorScreenPos();
                    // clamp vs a zero avail width: InvisibleButton asserts on a zero axis
                    const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
                    const ImRect bb(pos, ImVec2(pos.x + width, pos.y + rowH));
                    // the queue pill sits on top of the row button
                    ImGui::SetNextItemAllowOverlap();
                    ImGui::InvisibleButton("##row", ImVec2(width, rowH));
                    const ImVec2 cursorAfterRow = ImGui::GetCursorScreenPos();
                    const bool hovered = ImGui::IsItemHovered();
                    const bool held = ImGui::IsItemActive();
                    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
                    const bool added = groupActive(e.type);

                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    // expressive row: a rounded surface, the accent container when
                    // the effect is queued, a hover layer otherwise.
                    const float rowRadius = ImGuiM3Radius(ImGuiM3Shape_Large);
                    const ImGuiM3ShapeRounding r{ rowRadius, rowRadius, rowRadius, rowRadius };
                    ImGuiM3PathRoundedRect(dl, bb, r, added ? ImGuiM3ColorU32(ImGuiM3Role_SecondaryContainer)
                                                            : ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainer));
                    const ImGuiM3State st = (hovered && held) ? ImGuiM3State_Pressed : hovered ? ImGuiM3State_Hovered : ImGuiM3State_Enabled;
                    ImGuiM3DrawStateLayer(dl, bb, r, added ? ImGuiM3Role_OnSecondaryContainer : ImGuiM3Role_OnSurface, st);

                    if (iconFont)
                    {
                        char iconUtf8[8] = {};
                        const int iconLen = ImTextCharToUtf8(iconUtf8, (unsigned int)effectIconCodepointFor(e.type));
                        iconUtf8[iconLen] = '\0';
                        ImGuiM3DrawIcon(dl, iconUtf8, ImRect(ImVec2(bb.Min.x + 16.0f * d, bb.Min.y), ImVec2(bb.Min.x + 44.0f * d, bb.Max.y)),
                                        22.0f * d, ImGuiM3ColorU32(added ? ImGuiM3Role_OnSecondaryContainer : ImGuiM3Role_Primary));
                    }

                    // right cluster: [-] total count [+], then the 56dp switch.
                    // Measure it first so the name and path can never run under it.
                    const ImGuiM3Metrics& rowMetrics = ImGuiM3GetMetrics();
                    const float switchW = rowMetrics.switch_track_width * d;
                    const float switchH = rowMetrics.list_item_height_1 * d;
                    const float switchX = bb.Max.x - switchW - 14.0f * d;
                    const float gap = 10.0f * d;
                    const float stepSize = 32.0f * d;
                    const int copyCount = copiesFor(e.type);
                    char countText[12];
                    std::snprintf(countText, sizeof(countText), "%d", copyCount);
                    const float countW = ImGui::CalcTextSize(countText).x;
                    const float stepperW = stepSize * 2.0f + countW + gap * 2.0f;
                    const float controlsW = switchW + 14.0f * d + (added ? gap + stepperW : 0.0f);
                    bool overControl = false;

                    // two-line block: the effect name over its shader path
                    const float textX = bb.Min.x + 54.0f * d;
                    const float textW = ImMax(1.0f, bb.Max.x - controlsW - 8.0f * d - textX);
                    ImFont* brandFont = ImGuiM3FontBold();
                    ImFont* pathFont = ImGuiM3FontBodyBold();
                    const float namePx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodyLarge).size * d;
                    const float pathPx = ImGuiM3GetTypeStyle(ImGuiM3Type_BodySmall).size * d;
                    const char* pathText = e.path.c_str();
                    const bool showText = textW > 12.0f * d;
                    ImGui::PushFont(brandFont, namePx);
                    const char* shownName = showText ? UI::TruncateLabel(e.type.c_str(), textW) : "";
                    const ImVec2 nameSize = ImGui::CalcTextSize(shownName);
                    ImGui::PopFont();
                    ImGui::PushFont(pathFont, pathPx);
                    const char* shownPath = showText ? UI::TruncateLabel(pathText, textW) : "";
                    const ImVec2 pathSize = ImGui::CalcTextSize(shownPath);
                    ImGui::PopFont();
                    const bool hasPath = showText && pathText[0] != '\0';
                    const float blockH = nameSize.y + (hasPath ? pathSize.y + 2.0f * d : 0.0f);
                    const float nameY = bb.Min.y + (rowH - blockH) * 0.5f;
                    const ImU32 nameCol = ImGuiM3ColorU32(added ? ImGuiM3Role_OnSecondaryContainer
                                                                : ImGuiM3Role_OnSurface);
                    const ImU32 pathCol = ImGuiM3ColorU32(added ? ImGuiM3Role_OnSecondaryContainer
                                                                : ImGuiM3Role_OnSurfaceVariant);
                    dl->AddText(brandFont, namePx, ImVec2(textX, nameY), nameCol, shownName);
                    if (hasPath)
                        dl->AddText(pathFont, pathPx, ImVec2(textX, nameY + nameSize.y + 2.0f * d), pathCol,
                                    shownPath);

                    if (added)
                    {
                        const int extraCount = extraCopiesFor(e.type);
                        const float plusX = switchX - gap - stepSize;
                        const float countX = plusX - gap - countW;
                        const float minusX = countX - gap - stepSize;
                        const float controlY = bb.Min.y + (rowH - stepSize) * 0.5f;

                        ImGui::SetCursorScreenPos(ImVec2(minusX, controlY));
                        ImGui::PushID(3);
                        ImGui::SetNextItemAllowOverlap();
                        ImGui::BeginDisabled(extraCount == 0);
                        if (ImGui::M3Button(Icon::MinusUtf8, ImGuiM3Button_Outlined, ImVec2(stepSize, stepSize)))
                        {
                            addEffectsDuplicateCount[e.type] = std::max(0, extraCount - 1);
                            toggleQueued(e.type, true);
                        }
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        {
                            ImGui::SetTooltip("Remove one extra copy");
                            overControl = true;
                        }
                        ImGui::PopID();

                        ImGui::PushStyleColor(ImGuiCol_Text, ImGuiM3ColorU32(ImGuiM3Role_OnSecondaryContainer));
                        ImGui::SetCursorScreenPos(ImVec2(countX, bb.Min.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f));
                        ImGui::TextUnformatted(countText);
                        ImGui::PopStyleColor();

                        ImGui::SetCursorScreenPos(ImVec2(plusX, controlY));
                        ImGui::PushID(2);
                        ImGui::SetNextItemAllowOverlap();
                        ImGui::BeginDisabled(atLimit);
                        if (ImGui::M3Button(Icon::AddUtf8, ImGuiM3Button_Filled, ImVec2(stepSize, stepSize)))
                        {
                            addEffectsDuplicateCount[e.type] = std::min(static_cast<int>(maxEffectsLimit), extraCount + 1);
                            toggleQueued(e.type, true);
                        }
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        {
                            ImGui::SetTooltip(atLimit ? "Remove an effect or pending copy to free a slot"
                                                      : "Add one extra copy");
                            overControl = true;
                        }
                        ImGui::PopID();
                    }

                    bool queued = added;
                    ImGui::SetCursorScreenPos(ImVec2(switchX, bb.Min.y + (rowH - switchH) * 0.5f));
                    ImGui::PushID(1);
                    ImGui::SetNextItemAllowOverlap();
                    if (ImGui::M3SwitchWithID("", "##queue", &queued))
                        toggleQueued(e.type, queued);
                    if (ImGui::IsItemHovered())
                    {
                        overControl = true;
                        ImGui::SetTooltip(copyCount == 1
                            ? "Add or remove this copy."
                            : "Add or remove this effect and all %d copies together.", copyCount);
                    }
                    ImGui::PopID();
                    // restore the row's layout cursor; the Dummy consumes the
                    // SetCursorScreenPos so ImGui's boundary check is satisfied
                    ImGui::SetCursorScreenPos(cursorAfterRow);
                    ImGui::Dummy(ImVec2(0.0f, 0.0f));

                    if (hovered)
                    {
                        highlight = i;
                        if (!overControl && !e.path.empty())
                            ImGui::SetTooltip("%s", e.path.c_str());
                    }
                    if (clicked && !overControl && !added)
                        queueEffect(e.type);

                    ImGui::PopID();
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopClipRect();
        // only the x the slide pushed the cursor to: EndChild already advanced past
        // the results, and resetting both axes left the tray over the first rows
        ImGui::SetCursorScreenPos(ImVec2(resultsPos.x, ImGui::GetCursorScreenPos().y));

        // ---- Queued tray ----------------------------------------------------
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * d, 8.0f * d));
        ImGui::BeginChild("##queued", ImVec2(0, trayHeight), true);
        if (pendingAddEffects.empty())
        {
            UI::TextSupport("Queued effects appear here. Use a row switch or + to add copies.");
        }
        else
        {
            const float avail = ImMax(1.0f, ImGui::GetContentRegionAvail().x);
            float x = 0.0f;
            float y = 0.0f;
            for (size_t i = 0; i < pendingAddEffects.size(); i++)
            {
                ImGui::PushID(static_cast<int>(i));
                const std::string label = pendingAddEffects[i].first + "  " + Icon::CloseUtf8;
                const float chipW = ImMin(ImGui::M3ButtonWidth(label.c_str()), avail);
                if (x > 0.0f && x + chipW > avail)
                {
                    x = 0.0f;
                    y += trayChipH + ImGui::GetStyle().ItemSpacing.y;
                }
                ImGui::SetCursorPos(ImVec2(x, y));
                const float labelBudget = ImMax(1.0f, chipW - m.button_padding_x * 2.0f * d);
                const char* shownLabel = UI::TruncateLabel(label.c_str(), labelBudget);
                if (ImGui::M3Button(shownLabel, ImGuiM3Button_Tonal, ImVec2(chipW, trayChipH)))
                {
                    const std::string removedType = pendingAddEffects[i].second;
                    pendingAddEffects.erase(pendingAddEffects.begin() + i);
                    const int retained = alreadyInConfig(removedType) &&
                                         addEffectsRemoved.count(removedType) == 0 ? 1 : 0;
                    addEffectsDuplicateCount[removedType] =
                        std::max(0, retained + pendingCountFor(removedType) - 1);
                    ImGui::PopID();
                    break;
                }
                x += chipW + ImGui::GetStyle().ItemSpacing.x;
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        // ---- Footer ---------------------------------------------------------
        const std::string cancelLabel = std::string(Icon::CloseUtf8) + "  Cancel";
        const bool hasRemovals = !addEffectsRemoved.empty();
        const size_t queuedNow = pendingAddEffects.size();
        std::string applyLabel = std::string(Icon::CheckUtf8) + "  ";
        if (hasRemovals)
            applyLabel += "Apply";
        else if (queuedNow > 0)
            applyLabel += "Add " + std::to_string(queuedNow) + (queuedNow == 1 ? " effect" : " effects");
        else
            applyLabel += "Add";

        // flush right, and clamped up so the pills clear the bottom of the content:
        // the reserved height left them a spacing past it, cropping the fill
        const float actionH = m.button_height_default * d;
        const float actionGap = 10.0f * d;
        const float cancelW = ImGui::M3ButtonWidth(cancelLabel.c_str());
        const float applyW = ImGui::M3ButtonWidth(applyLabel.c_str());
        const float rowRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImMax(ImGui::GetCursorPosX(), rowRight - cancelW - actionGap - applyW));
        const float rowBottom = ImGui::GetCursorPosY() + ImGui::GetContentRegionAvail().y;
        ImGui::SetCursorPosY(ImMin(ImGui::GetCursorPosY(), rowBottom - actionH));

        if (ImGui::M3Button(cancelLabel.c_str(), ImGuiM3Button_Outlined))
        {
            pendingAddEffects.clear();
            addEffectsDuplicateCount.clear();
            addEffectsRemoved.clear();
            insertPosition = -1;
            inSelectionMode = false;
            addEffectsSearch[0] = '\0';
        }
        ImGui::SameLine(0.0f, actionGap);
        ImGui::BeginDisabled(pendingAddEffects.empty() && addEffectsRemoved.empty());
        if (ImGui::M3Button(applyLabel.c_str(), ImGuiM3Button_Filled))
        {
            // removals first, so an insert position lines up with what is left
            bool removedAny = false;
            if (hasRemovals)
            {
                for (const auto& removed : addEffectsRemoved)
                {
                    auto it = std::find(selectedEffects.begin(), selectedEffects.end(), removed);
                    if (it == selectedEffects.end())
                        continue;
                    effectRegistry->removeEffect(*it);
                    selectedEffects.erase(it);
                    removedAny = true;
                }
                if (removedAny)
                    effectRegistry->setSelectedEffects(selectedEffects);
            }

            int pos = (insertPosition >= 0 && insertPosition <= static_cast<int>(selectedEffects.size()))
                      ? insertPosition : static_cast<int>(selectedEffects.size());
            for (const auto& [instanceName, effectType] : pendingAddEffects)
            {
                selectedEffects.insert(selectedEffects.begin() + pos, instanceName);
                pos++;
                effectRegistry->ensureEffect(instanceName, effectType);
                effectRegistry->setEffectEnabled(instanceName, true);
            }
            const bool addedAny = !pendingAddEffects.empty();
            if (addedAny)
                effectRegistry->setSelectedEffects(selectedEffects);

            if (removedAny || addedAny)
            {
                paramsDirty = false;
                reloadNeeded = false;
                applyRequested = true;
                profileDirty = true;
            }
            pendingAddEffects.clear();
            addEffectsDuplicateCount.clear();
            addEffectsRemoved.clear();
            insertPosition = -1;
            inSelectionMode = false;
            addEffectsSearch[0] = '\0';
            highlight = 0;
        }
        ImGui::EndDisabled();
    }

} // namespace VKIntox
