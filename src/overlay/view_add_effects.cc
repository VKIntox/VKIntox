#include "imgui_overlay.hh"
#include "effects/effect_registry.hh"
#include "settings_manager.hh"
#include "reshade_parser.hh"
#include "config_serializer.hh"

#include <algorithm>
#include <cstring>
#include <cctype>
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

    // A visual hint about what an effect does, picked from its name.
    static const char* effectIconFor(const std::string& name)
    {
        const std::string n = toLower(name);
        auto has = [&](const char* needle) { return n.find(needle) != std::string::npos; };

        if (has("lut") || has("color") || has("curve") || has("grade")) return Icon::ColorizeUtf8;
        if (has("blur") || has("dof") || has("bokeh"))                   return Icon::BlurOnUtf8;
        if (has("film") || has("grain") || has("noise") || has("deb"))   return Icon::GrainUtf8;
        if (has("sharp") || has("cas") || has("aa"))                     return Icon::StraightenUtf8;
        if (has("bloom") || has("glow") || has("light"))                 return Icon::BoltUtf8;
        if (has("tone") || has("contrast") || has("hdr"))                return Icon::ContrastUtf8;
        if (has("depth") || has("ssao") || has("ssr") || has("ao"))      return Icon::LayersUtf8;
        if (has("dls"))                                                  return Icon::AutoAwesomeUtf8;
        if (has("shader") || has("fx") || has("effect"))                 return Icon::BrushUtf8;
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
        // inputs rarely move, so cache the result until state, search or filter
        // actually changes.
        static int filter = 0;   // 0 all, 1 built-in, 2 this config, 3 reshade
        if (overlayStateVersion != addEffectsCacheVersion
            || std::string(addEffectsSearch) != addEffectsCacheSearch
            || filter != addEffectsCacheFilter)
        {
            addEffectsEntries.clear();

            static const char* const builtinEffects[] = {"cas", "dls", "fxaa", "smaa", "deband", "lut"};
            std::vector<std::string> sortedCurrent = state.currentConfigEffects;
            std::vector<std::string> sortedDefault = state.defaultConfigEffects;
            std::sort(sortedCurrent.begin(), sortedCurrent.end());
            std::sort(sortedDefault.begin(), sortedDefault.end());

            auto addEntry = [&](const std::string& type, const std::string& path, int group) {
                const int score = matchScore(type, addEffectsSearch);
                if (score >= 0)
                    addEffectsEntries.push_back({type, path, group, score});
            };
            for (const char* et : builtinEffects)
                addEntry(et, "", 0);
            for (const auto& et : sortedCurrent)
            {
                auto it = state.effectPaths.find(et);
                addEntry(et, (it != state.effectPaths.end()) ? it->second : "", 1);
            }
            for (const auto& et : sortedDefault)
            {
                if (std::find(sortedCurrent.begin(), sortedCurrent.end(), et) != sortedCurrent.end())
                    continue;
                auto it = state.effectPaths.find(et);
                addEntry(et, (it != state.effectPaths.end()) ? it->second : "", 2);
            }

            if (filter != 0)
                addEffectsEntries.erase(std::remove_if(addEffectsEntries.begin(), addEffectsEntries.end(),
                                                       [&](const AddEffectsEntry& e) { return e.group != filter - 1; }),
                                        addEffectsEntries.end());

            std::stable_sort(addEffectsEntries.begin(), addEffectsEntries.end(), [](const AddEffectsEntry& a, const AddEffectsEntry& b) {
                if (a.score != b.score)
                    return a.score < b.score;
                if (a.group != b.group)
                    return a.group < b.group;
                return a.type < b.type;
            });

            addEffectsCacheVersion = overlayStateVersion;
            addEffectsCacheSearch = addEffectsSearch;
            addEffectsCacheFilter = filter;
        }
        const std::vector<AddEffectsEntry>& entries = addEffectsEntries;

        const int entryCount = static_cast<int>(entries.size());
        const bool hasSearch = addEffectsSearch[0] != '\0';

        // Keyboard cursor.
        static int highlight = 0;
        if (entryCount == 0)
            highlight = 0;
        else if (highlight >= entryCount)
            highlight = entryCount - 1;

        if (entryCount > 0)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                highlight = std::min(highlight + 1, entryCount - 1);
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
            if (submitted && entryCount > 0)
                queueEffect(entries[highlight].type);
        }

        // Filter chips.
        {
            const std::pair<const char*, int> chips[] = {
                {"All", 0}, {"Built-in", 1}, {"This config", 2}, {"ReShade", 3}};
            for (int i = 0; i < (int)(sizeof(chips) / sizeof(chips[0])); i++)
            {
                if (i > 0)
                    ImGui::SameLine();
                const bool selected = (filter == chips[i].second);
                if (ImGui::M3Button(chips[i].first,
                                    selected ? ImGuiM3Button_Tonal : ImGuiM3Button_Text,
                                    ImVec2(0, 30.0f * d)))
                    filter = chips[i].second;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%d result%s", entryCount, entryCount == 1 ? "" : "s");
        }

        // Recents (only when browsing, not searching).
        if (!hasSearch && !recent.empty())
        {
            ImGui::TextDisabled("Recent:");
            for (const std::string& r : recent)
            {
                ImGui::SameLine();
                ImGui::PushID(r.c_str());
                if (ImGui::M3Button(r.c_str(), ImGuiM3Button_Outlined, ImVec2(0, 26.0f * d)))
                    queueEffect(r);
                ImGui::PopID();
            }
        }

        // ---- Results (virtualized) -----------------------------------------
        const float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        const float trayHeight = 62.0f * d;
        const float rowH = 40.0f * d;
        ImFont* iconFont = ImGuiM3IconFont();
        ImFont* medium = ImGuiM3FontMedium();

        ImGui::BeginChild("##results", ImVec2(0, -footerHeight - trayHeight), true);
        if (entryCount == 0)
        {
            ImGui::Spacing();
            ImGui::TextDisabled("%s", Icon::SearchUtf8);
            ImGui::TextDisabled(hasSearch ? "No effects match this filter." : "No effects available.");
        }
        else
        {
            ImGuiListClipper clipper;
            clipper.Begin(entryCount, rowH);
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
                {
                    const AddEffectsEntry& e = entries[i];
                    ImGui::PushID(i);
                    const ImVec2 pos = ImGui::GetCursorScreenPos();
                    const float width = ImGui::GetContentRegionAvail().x;
                    const ImRect bb(pos, ImVec2(pos.x + width, pos.y + rowH));
                    ImGui::InvisibleButton("##row", ImVec2(width, rowH - 2.0f));
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
                    ImGui::RenderTextClipped(ImVec2(bb.Min.x + 50.0f * d, bb.Min.y), ImVec2(bb.Max.x - 150.0f * d, bb.Max.y),
                                             e.type.c_str(), NULL, NULL, ImVec2(0.0f, 0.5f), &bb);
                    ImGui::PopStyleColor();
                    if (medium)
                        ImGui::PopFont();

                    const char* group = e.group == 0 ? "built-in" : e.group == 1 ? "config" : "reshade";
                    ImGui::RenderTextClipped(ImVec2(bb.Max.x - 150.0f * d, bb.Min.y), ImVec2(bb.Max.x - 40.0f * d, bb.Max.y),
                                             group, NULL, NULL, ImVec2(1.0f, 0.5f), &bb);

                    if (iconFont)
                        ImGuiM3DrawIcon(dl, added ? Icon::CheckCircleUtf8 : Icon::AddUtf8,
                                        ImRect(ImVec2(bb.Max.x - 40.0f * d, bb.Min.y), ImVec2(bb.Max.x - 12.0f * d, bb.Max.y)),
                                        20.0f * d, ImGuiM3ColorU32(added ? ImGuiM3Role_Tertiary : ImGuiM3Role_Primary));

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
