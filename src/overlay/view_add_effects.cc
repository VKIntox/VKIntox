#include "imgui_overlay.hh"
#include "effects/effect_registry.hh"
#include "settings_manager.hh"
#include "reshade_parser.hh"
#include "config_serializer.hh"

#include <algorithm>
#include <cstring>
#include <cctype>
#include <set>
#include <string>
#include <vector>

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imgui_internal.h"
#include "vendor/imgui/imgui_m3.h"
#include "overlay/ui_theme.hh"
#include "overlay/ui_icons.hh"

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

    // A visual hint about what an effect does, picked from its name. First match
    // wins, so the more specific hints come before the broad ones.
    static const char* effectIconFor(const std::string& name)
    {
        const std::string n = toLower(name);
        auto has = [&](const char* needle) { return n.find(needle) != std::string::npos; };

        if (has("motionblur") || has("motion_blur") || has("motion blur")) return Icon::MotionBlurUtf8;
        if (has("sharpen") || has("sharp") || has("clarity"))             return Icon::DiamondShineUtf8;
        if (has("dof") || has("bokeh"))                                   return Icon::LensBlurUtf8;
        if (has("ssr"))                                                   return Icon::GradientUtf8;
        if (has("bloom") || has("flare") || has("glow"))                  return Icon::FlareUtf8;
        if (has("lut") || has("color") || has("curve") || has("grade"))   return Icon::ColorizeUtf8;
        if (has("blur"))                                                  return Icon::BlurOnUtf8;
        if (has("film") || has("grain") || has("noise") || has("deb"))    return Icon::GrainUtf8;
        if (has("cas") || has("aa"))                                      return Icon::StraightenUtf8;
        if (has("light"))                                                 return Icon::BoltUtf8;
        if (has("tone") || has("contrast") || has("hdr"))                 return Icon::ContrastUtf8;
        if (has("ssao") || has("mxao"))                                   return Icon::ShadowUtf8;
        if (has("depth"))                                                 return Icon::LayersUtf8;
        if (has("shader") || has("fx") || has("effect"))                  return Icon::BrushUtf8;
        return Icon::ExtensionUtf8;
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
        const size_t pendingCount = pendingAddEffects.size();
        const bool atLimit = currentCount + pendingCount >= maxEffectsLimit;

        auto isNameUsed = [&](const std::string& name) {
            if (std::find(selectedEffects.begin(), selectedEffects.end(), name) != selectedEffects.end())
                return true;
            for (const auto& p : pendingAddEffects)
                if (p.first == name)
                    return true;
            return false;
        };
        auto isPending = [&](const std::string& effectType) {
            for (const auto& p : pendingAddEffects)
                if (p.second == effectType)
                    return true;
            return false;
        };
        auto getNextInstanceName = [&](const std::string& effectType) -> std::string {
            if (!isNameUsed(effectType))
                return effectType;
            for (int n = 2; n <= 99; n++)
            {
                std::string candidate = effectType + "." + std::to_string(n);
                if (!isNameUsed(candidate))
                    return candidate;
            }
            return effectType + ".99";
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
        auto queueEffect = [&](const std::string& effectType) {
            if (atLimit || isPending(effectType))
                return;
            pendingAddEffects.push_back({getNextInstanceName(effectType), effectType});
            pushRecent(effectType);
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

            // the tabs are the packages present in this set, so they follow installs
            {
                std::set<std::string> packages;
                for (const auto& e : addEffectsEntries)
                    if (!e.package.empty())
                        packages.insert(e.package);
                addEffectsPackages.assign(packages.begin(), packages.end());
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

        // ---- Header ---------------------------------------------------------
        ImGui::PushFont(ImGuiM3FontBold(), ImGui::GetFontSize());
        ImGui::Text("%s  %s", Icon::AddUtf8, insertPosition >= 0 ? "Insert Effects" : "Add Effects");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextColored(atLimit ? UI::Warning() : UI::Muted(), "%zu / %zu queued",
                           currentCount + pendingCount, maxEffectsLimit);
        if (insertPosition >= 0)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("at position %d", insertPosition);
        }

        // ---- Search ---------------------------------------------------------
        if (addEffectsFocusSearch)
        {
            ImGui::SetKeyboardFocusHere();
            addEffectsFocusSearch = false;
        }
        {
            const ImVec2 prevPad = ImGui::GetStyle().FramePadding;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(38.0f * d, prevPad.y));
            ImGui::SetNextItemWidth(-1);
            const bool submitted = ImGui::InputTextWithHint("##search", "Search effects (type to filter, Enter to add)...",
                                                            addEffectsSearch, sizeof(addEffectsSearch),
                                                            ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);
            const ImVec2 fmin = ImGui::GetItemRectMin();
            const ImVec2 fmax = ImGui::GetItemRectMax();
            ImFont* iconFont = ImGuiM3IconFont();
            if (iconFont)
                ImGuiM3DrawIcon(ImGui::GetWindowDrawList(), Icon::SearchUtf8,
                                ImRect(ImVec2(fmin.x + 8.0f * d, fmin.y), ImVec2(fmin.x + 30.0f * d, fmax.y)),
                                18.0f * d, ImGuiM3ColorU32(ImGuiM3Role_OnSurfaceVariant));
            ImGui::PopStyleVar();
            if (submitted && rowCount > 0 && rows[highlight].entry >= 0)
                queueEffect(entries[rows[highlight].entry].type);
        }

        // Package tabs: All plus one per installed effect package, in a
        // horizontally scrolling row so a long package list stays usable.
        {
            ImGui::BeginChild("##packagetabs", ImVec2(0, 46.0f * d), false, ImGuiWindowFlags_HorizontalScrollbar);
            auto tab = [&](const char* label, const std::string& package) {
                const bool selected = (filterPackage == package);
                if (ImGui::M3Button(label, selected ? ImGuiM3Button_Tonal : ImGuiM3Button_Text, ImVec2(0, 30.0f * d)))
                    filterPackage = package;
                ImGui::SameLine();
            };
            tab("All", std::string());
            for (const auto& package : addEffectsPackages)
                tab(package.c_str(), package);
            ImGui::TextDisabled("%d result%s", static_cast<int>(entries.size()), entries.size() == 1 ? "" : "s");

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
        const float trayHeight = 62.0f * d;
        const float rowH = 40.0f * d;
        ImFont* iconFont = ImGuiM3IconFont();
        ImFont* medium = ImGuiM3FontMedium();

        ImGui::BeginChild("##results", ImVec2(0, -footerHeight - trayHeight), true);
        if (rowCount == 0)
        {
            ImGui::Spacing();
            ImGui::TextDisabled("%s", Icon::SearchUtf8);
            ImGui::TextDisabled(hasSearch ? "No effects match this filter." : "No effects available.");
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
                    ImGui::InvisibleButton("##row", ImVec2(width, rowH - 2.0f));
                    const ImVec2 cursorAfterRow = ImGui::GetCursorScreenPos();
                    const bool hovered = ImGui::IsItemHovered();
                    const bool held = ImGui::IsItemActive();
                    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
                    const bool added = isPending(e.type);

                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const float pill = ImGuiM3PillRadius(bb.GetSize(), ImGuiM3Radius(ImGuiM3Shape_Full));
                    const ImGuiM3ShapeRounding r{pill, pill, pill, pill};
                    ImGuiM3PathRoundedRect(dl, bb, r, added ? ImGuiM3ColorU32(ImGuiM3Role_SecondaryContainer)
                                                            : ImGuiM3ColorU32(ImGuiM3Role_SurfaceContainerLow));
                    const ImGuiM3State st = (hovered && held) ? ImGuiM3State_Pressed : hovered ? ImGuiM3State_Hovered : ImGuiM3State_Enabled;
                    ImGuiM3DrawStateLayer(dl, bb, r, added ? ImGuiM3Role_OnSecondaryContainer : ImGuiM3Role_OnSurface, st);
                    if (i == highlight && !hovered)
                        dl->AddRect(ImVec2(bb.Min.x + d, bb.Min.y + d), ImVec2(bb.Max.x - d, bb.Max.y - d),
                                    ImGuiM3ColorU32(ImGuiM3Role_Primary), pill, 0, 2.0f * d);

                    if (iconFont)
                        ImGuiM3DrawIcon(dl, effectIconFor(e.type), ImRect(ImVec2(bb.Min.x + 16.0f * d, bb.Min.y), ImVec2(bb.Min.x + 44.0f * d, bb.Max.y)),
                                        20.0f * d, ImGuiM3ColorU32(added ? ImGuiM3Role_OnSecondaryContainer : ImGuiM3Role_Primary));

                    if (medium)
                        ImGui::PushFont(medium, ImGui::GetFontSize());
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGuiM3ColorU32(added ? ImGuiM3Role_OnSecondaryContainer : ImGuiM3Role_OnSurface));
                    ImGui::RenderTextClipped(ImVec2(bb.Min.x + 50.0f * d, bb.Min.y), ImVec2(bb.Max.x - 120.0f * d, bb.Max.y),
                                             e.type.c_str(), NULL, NULL, ImVec2(0.0f, 0.5f), &bb);
                    ImGui::PopStyleColor();
                    if (medium)
                        ImGui::PopFont();

                    // queue switch: on queues the effect, off removes it again
                    const ImGuiM3Metrics& rowMetrics = ImGuiM3GetMetrics();
                    const float switchW = rowMetrics.switch_track_width * d;
                    const float switchH = rowMetrics.list_item_height_1 * d;
                    bool queued = added;
                    ImGui::SetCursorScreenPos(ImVec2(bb.Max.x - switchW - 14.0f * d, bb.Min.y + (rowH - switchH) * 0.5f));
                    ImGui::PushID(1);
                    if (ImGui::M3SwitchWithID("", "##queue", &queued))
                    {
                        if (queued)
                            queueEffect(e.type);
                        else
                        {
                            auto pending = std::find_if(pendingAddEffects.begin(), pendingAddEffects.end(),
                                                        [&](const auto& p) { return p.second == e.type; });
                            if (pending != pendingAddEffects.end())
                                pendingAddEffects.erase(pending);
                        }
                    }
                    ImGui::PopID();
                    // restore the row's layout cursor; the Dummy consumes the
                    // SetCursorScreenPos so ImGui's boundary check is satisfied
                    ImGui::SetCursorScreenPos(cursorAfterRow);
                    ImGui::Dummy(ImVec2(0.0f, 0.0f));

                    if (hovered)
                    {
                        highlight = i;
                        if (!e.path.empty())
                            ImGui::SetTooltip("%s", e.path.c_str());
                    }
                    if (clicked)
                        queueEffect(e.type);

                    ImGui::PopID();
                }
            }
        }
        ImGui::EndChild();

        // ---- Queued tray ----------------------------------------------------
        ImGui::BeginChild("##queued", ImVec2(0, trayHeight), true);
        if (pendingAddEffects.empty())
        {
            ImGui::TextDisabled("Queued effects appear here. Click a result or press Enter to queue it.");
        }
        else
        {
            const float avail = ImGui::GetContentRegionAvail().x;
            float x = 0.0f;
            for (size_t i = 0; i < pendingAddEffects.size(); i++)
            {
                ImGui::PushID(static_cast<int>(i));
                const std::string label = pendingAddEffects[i].first + "  " + Icon::CloseUtf8;
                const float chipW = ImGui::CalcTextSize(label.c_str()).x + 24.0f * d;
                if (x > 0.0f && x + chipW > avail)
                    x = 0.0f;
                else if (x > 0.0f)
                    ImGui::SameLine();
                if (ImGui::Button(label.c_str()))
                {
                    pendingAddEffects.erase(pendingAddEffects.begin() + i);
                    ImGui::PopID();
                    break;
                }
                x += chipW + ImGui::GetStyle().ItemSpacing.x;
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        // ---- Footer ---------------------------------------------------------
        const std::string cancelLabel = std::string(Icon::CloseUtf8) + "  Cancel";
        std::string addLabel = std::string(Icon::CheckUtf8) + "  ";
        addLabel += pendingCount == 0 ? "Add" : ("Add " + std::to_string(pendingCount) + (pendingCount == 1 ? " Effect" : " Effects"));

        if (ImGui::M3Button(cancelLabel.c_str(), ImGuiM3Button_Outlined))
        {
            pendingAddEffects.clear();
            insertPosition = -1;
            inSelectionMode = false;
            addEffectsSearch[0] = '\0';
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(pendingAddEffects.empty());
        if (ImGui::M3Button(addLabel.c_str(), ImGuiM3Button_Filled))
        {
            int pos = (insertPosition >= 0 && insertPosition <= static_cast<int>(selectedEffects.size()))
                      ? insertPosition : static_cast<int>(selectedEffects.size());
            for (const auto& [instanceName, effectType] : pendingAddEffects)
            {
                selectedEffects.insert(selectedEffects.begin() + pos, instanceName);
                pos++;
                effectRegistry->ensureEffect(instanceName, effectType);
                effectRegistry->setEffectEnabled(instanceName, true);
            }
            if (!pendingAddEffects.empty())
            {
                effectRegistry->setSelectedEffects(selectedEffects);
                applyRequested = true;
                profileDirty = true;
            }
            pendingAddEffects.clear();
            insertPosition = -1;
            inSelectionMode = false;
            addEffectsSearch[0] = '\0';
            highlight = 0;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Enter queues the highlighted result");
    }

} // namespace VKIntox
