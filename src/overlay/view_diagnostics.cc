#include "imgui_overlay.hh"
#include "config_serializer.hh"
#include "logger.hh"
#include "overlay/ui_theme.hh"
#include "overlay/ui_icons.hh"

#include <fstream>
#include <filesystem>
#include <chrono>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <dlfcn.h>

#include "vendor/imgui/imgui.h"

namespace VKIntox
{
    namespace
    {
        // Ring buffer for storing history
        template<typename T, size_t N>
        class RingBuffer
        {
        public:
            void push(T value)
            {
                data[writeIndex] = value;
                writeIndex = (writeIndex + 1) % N;
                if (count < N)
                    count++;
            }

            T get(size_t i) const
            {
                if (i >= count)
                    return T{};
                size_t idx = (writeIndex + N - count + i) % N;
                return data[idx];
            }

            size_t size() const { return count; }
            static constexpr size_t capacity() { return N; }

            T min() const
            {
                if (count == 0) return T{};
                T m = get(0);
                for (size_t i = 1; i < count; i++)
                    m = std::min(m, get(i));
                return m;
            }

            T max() const
            {
                if (count == 0) return T{};
                T m = get(0);
                for (size_t i = 1; i < count; i++)
                    m = std::max(m, get(i));
                return m;
            }

            T avg() const
            {
                if (count == 0) return T{};
                T sum = T{};
                for (size_t i = 0; i < count; i++)
                    sum += get(i);
                return sum / static_cast<T>(count);
            }

            // Get data as contiguous array for ImGui plotting
            void copyTo(float* out) const
            {
                for (size_t i = 0; i < count; i++)
                    out[i] = static_cast<float>(get(i));
            }

        private:
            T data[N] = {};
            size_t writeIndex = 0;
            size_t count = 0;
        };

        // ── GPU vendor detection ────────────────────────────────────────────

        enum class GpuVendor { Unknown, AMD, Intel, NVIDIA };

        struct GpuInfo
        {
            GpuVendor vendor = GpuVendor::Unknown;
            std::string drmCardPath;      // /sys/class/drm/cardN
            std::string vendorName;       // Display name
            bool hasGpuUsage = false;
            bool hasVram = false;
            bool hasGtt = false;
        };

        // ── NVIDIA NVML (runtime dlopen) ────────────────────────────────────

        // NVML types (from nvml.h, but we don't require the header)
        using nvmlReturn_t = unsigned int;
        using nvmlDevice_t = void*;
        struct nvmlUtilization_t { unsigned int gpu; unsigned int memory; };
        struct nvmlMemory_t { unsigned long long total; unsigned long long free; unsigned long long used; };

        static constexpr nvmlReturn_t NVML_SUCCESS = 0;

        struct NvmlState
        {
            void* lib = nullptr;
            nvmlDevice_t device = nullptr;
            bool initialized = false;

            // Function pointers
            nvmlReturn_t (*Init)() = nullptr;
            nvmlReturn_t (*Shutdown)() = nullptr;
            nvmlReturn_t (*DeviceGetHandleByIndex)(unsigned int, nvmlDevice_t*) = nullptr;
            nvmlReturn_t (*DeviceGetUtilizationRates)(nvmlDevice_t, nvmlUtilization_t*) = nullptr;
            nvmlReturn_t (*DeviceGetMemoryInfo)(nvmlDevice_t, nvmlMemory_t*) = nullptr;
            nvmlReturn_t (*DeviceGetCount)(unsigned int*) = nullptr;
        };

        static NvmlState nvml;

        static bool initNvml()
        {
            if (nvml.initialized)
                return nvml.lib != nullptr;
            nvml.initialized = true;

            nvml.lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
            if (!nvml.lib)
                nvml.lib = dlopen("libnvidia-ml.so", RTLD_LAZY);
            if (!nvml.lib)
            {
                Logger::debug("NVML not available: " + std::string(dlerror()));
                return false;
            }

            nvml.Init = (decltype(nvml.Init))dlsym(nvml.lib, "nvmlInit_v2");
            if (!nvml.Init)
                nvml.Init = (decltype(nvml.Init))dlsym(nvml.lib, "nvmlInit");
            nvml.Shutdown = (decltype(nvml.Shutdown))dlsym(nvml.lib, "nvmlShutdown");
            nvml.DeviceGetHandleByIndex = (decltype(nvml.DeviceGetHandleByIndex))dlsym(nvml.lib, "nvmlDeviceGetHandleByIndex_v2");
            if (!nvml.DeviceGetHandleByIndex)
                nvml.DeviceGetHandleByIndex = (decltype(nvml.DeviceGetHandleByIndex))dlsym(nvml.lib, "nvmlDeviceGetHandleByIndex");
            nvml.DeviceGetUtilizationRates = (decltype(nvml.DeviceGetUtilizationRates))dlsym(nvml.lib, "nvmlDeviceGetUtilizationRates");
            nvml.DeviceGetMemoryInfo = (decltype(nvml.DeviceGetMemoryInfo))dlsym(nvml.lib, "nvmlDeviceGetMemoryInfo");
            nvml.DeviceGetCount = (decltype(nvml.DeviceGetCount))dlsym(nvml.lib, "nvmlDeviceGetCount_v2");
            if (!nvml.DeviceGetCount)
                nvml.DeviceGetCount = (decltype(nvml.DeviceGetCount))dlsym(nvml.lib, "nvmlDeviceGetCount");

            if (!nvml.Init || !nvml.DeviceGetHandleByIndex || !nvml.DeviceGetUtilizationRates || !nvml.DeviceGetMemoryInfo)
            {
                Logger::debug("NVML: missing required symbols");
                dlclose(nvml.lib);
                nvml.lib = nullptr;
                return false;
            }

            if (nvml.Init() != NVML_SUCCESS)
            {
                Logger::debug("NVML: nvmlInit failed");
                dlclose(nvml.lib);
                nvml.lib = nullptr;
                return false;
            }

            // Get first GPU handle (index 0)
            if (nvml.DeviceGetHandleByIndex(0, &nvml.device) != NVML_SUCCESS)
            {
                Logger::debug("NVML: could not get device handle");
                nvml.Shutdown();
                dlclose(nvml.lib);
                nvml.lib = nullptr;
                return false;
            }

            Logger::info("NVML initialized for GPU diagnostics");
            return true;
        }

        __attribute__((unused)) static void shutdownNvml()
        {
            if (nvml.lib)
            {
                if (nvml.Shutdown)
                    nvml.Shutdown();
                dlclose(nvml.lib);
                nvml.lib = nullptr;
            }
        }

        // ── GPU discovery ───────────────────────────────────────────────────

        // Read PCI vendor ID from DRM card sysfs
        static uint16_t readVendorId(const std::string& cardPath)
        {
            std::ifstream f(cardPath + "/device/vendor");
            if (!f.is_open())
                return 0;
            uint16_t id = 0;
            f >> std::hex >> id;
            return id;
        }

        static GpuInfo findGpu(const DeviceInfo& device)
        {
            GpuInfo info;

            // the DRM node for the slot the game is actually on. on hybrid
            // laptops the first cardN is usually the iGPU, which is why this
            // used to report Intel while the game rendered on the dGPU.
            std::error_code ec;
            std::string wantedCard;
            if (!device.gpuPciSlot.empty())
            {
                for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm", ec))
                {
                    std::string name = entry.path().filename().string();
                    if (name.find("card") != 0 || name.find("-") != std::string::npos)
                        continue;
                    std::error_code linkEc;
                    std::string target = std::filesystem::canonical(entry.path() / "device", linkEc).string();
                    const size_t slash = target.rfind('/');
                    if (slash != std::string::npos && target.substr(slash + 1) == device.gpuPciSlot)
                    {
                        wantedCard = entry.path().string();
                        break;
                    }
                }
            }

            try
            {
                for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm"))
                {
                    std::string name = entry.path().filename().string();
                    if (name.find("card") != 0 || name.find("-") != std::string::npos)
                        continue;

                    std::string cardPath = entry.path().string();
                    // prefer the slot the game is on; fall back to any supported
                    // card only when the slot can't be resolved
                    if (!wantedCard.empty() && cardPath != wantedCard)
                        continue;

                    uint16_t vendorId = readVendorId(cardPath);
                    if (!wantedCard.empty() && vendorId != device.gpuVendorId)
                        continue;

                    // 0x1002 = AMD, 0x8086 = Intel, 0x10de = NVIDIA
                    if (vendorId == 0x1002)
                    {
                        // AMD: check for gpu_busy_percent
                        if (std::filesystem::exists(cardPath + "/device/gpu_busy_percent"))
                        {
                            info.vendor = GpuVendor::AMD;
                            info.drmCardPath = cardPath;
                            info.vendorName = "AMD";
                            info.hasGpuUsage = true;
                            info.hasVram = std::filesystem::exists(cardPath + "/device/mem_info_vram_total");
                            info.hasGtt = std::filesystem::exists(cardPath + "/device/mem_info_gtt_total");
                            return info;
                        }
                    }
                    else if (vendorId == 0x8086)
                    {
                        // Intel: use frequency ratio as GPU utilization estimate
                        info.vendor = GpuVendor::Intel;
                        info.drmCardPath = cardPath;
                        info.vendorName = "Intel";
                        info.hasGpuUsage = std::filesystem::exists(cardPath + "/device/gt_act_freq_mhz") &&
                                           std::filesystem::exists(cardPath + "/device/gt_max_freq_mhz");
                        // Intel discrete (Arc) may have VRAM via drm_memory_stats
                        info.hasVram = std::filesystem::exists(cardPath + "/device/mem_info_vram_total");
                        info.hasGtt = false;
                        return info;
                    }
                    else if (vendorId == 0x10de)
                    {
                        // NVIDIA: use NVML (runtime dlopen)
                        info.vendor = GpuVendor::NVIDIA;
                        info.drmCardPath = cardPath;
                        info.vendorName = "NVIDIA";
                        if (initNvml())
                        {
                            info.hasGpuUsage = true;
                            info.hasVram = true;
                        }
                        return info;
                    }
                }
            }
            catch (...) {}

            return info;
        }

        // ── Static state ────────────────────────────────────────────────────

        static RingBuffer<float, 300> frameTimeHistory;
        static RingBuffer<float, 300> gpuUsageHistory;
        static RingBuffer<float, 300> vramUsageHistory;
        static RingBuffer<float, 300> gttUsageHistory;
        static std::chrono::steady_clock::time_point lastFrameTime;
        static GpuInfo gpuInfo;
        static std::string detectedGameName;
        static std::string autoDetectedConfig;

        // Read a single value from sysfs
        template<typename T>
        bool readSysfs(const std::string& path, T& value)
        {
            std::ifstream file(path);
            if (!file.is_open())
                return false;
            file >> value;
            return !file.fail();
        }

        // ── Per-vendor stat readers ─────────────────────────────────────────

        float getGpuUsage()
        {
            if (gpuInfo.vendor == GpuVendor::AMD)
            {
                int usage = 0;
                if (readSysfs(gpuInfo.drmCardPath + "/device/gpu_busy_percent", usage))
                    return static_cast<float>(usage);
            }
            else if (gpuInfo.vendor == GpuVendor::Intel)
            {
                // Frequency ratio: (actual / max) * 100 as utilization estimate
                int actFreq = 0, maxFreq = 0;
                if (readSysfs(gpuInfo.drmCardPath + "/device/gt_act_freq_mhz", actFreq) &&
                    readSysfs(gpuInfo.drmCardPath + "/device/gt_max_freq_mhz", maxFreq) &&
                    maxFreq > 0)
                {
                    return std::min(100.0f, (static_cast<float>(actFreq) / static_cast<float>(maxFreq)) * 100.0f);
                }
            }
            else if (gpuInfo.vendor == GpuVendor::NVIDIA && nvml.lib)
            {
                nvmlUtilization_t util = {};
                if (nvml.DeviceGetUtilizationRates(nvml.device, &util) == NVML_SUCCESS)
                    return static_cast<float>(util.gpu);
            }

            return -1.0f;
        }

        bool getVramUsage(float& usedMB, float& totalMB)
        {
            if (gpuInfo.vendor == GpuVendor::AMD || gpuInfo.vendor == GpuVendor::Intel)
            {
                uint64_t used = 0, total = 0;
                if (readSysfs(gpuInfo.drmCardPath + "/device/mem_info_vram_used", used) &&
                    readSysfs(gpuInfo.drmCardPath + "/device/mem_info_vram_total", total) &&
                    total > 0)
                {
                    usedMB = static_cast<float>(used) / (1024.0f * 1024.0f);
                    totalMB = static_cast<float>(total) / (1024.0f * 1024.0f);
                    return true;
                }
            }
            else if (gpuInfo.vendor == GpuVendor::NVIDIA && nvml.lib)
            {
                nvmlMemory_t mem = {};
                if (nvml.DeviceGetMemoryInfo(nvml.device, &mem) == NVML_SUCCESS && mem.total > 0)
                {
                    usedMB = static_cast<float>(mem.used) / (1024.0f * 1024.0f);
                    totalMB = static_cast<float>(mem.total) / (1024.0f * 1024.0f);
                    return true;
                }
            }

            return false;
        }

        bool getGttUsage(float& usedMB, float& totalMB)
        {
            if (gpuInfo.vendor != GpuVendor::AMD || gpuInfo.drmCardPath.empty())
                return false;

            uint64_t used = 0, total = 0;
            if (readSysfs(gpuInfo.drmCardPath + "/device/mem_info_gtt_used", used) &&
                readSysfs(gpuInfo.drmCardPath + "/device/mem_info_gtt_total", total) &&
                total > 0)
            {
                usedMB = static_cast<float>(used) / (1024.0f * 1024.0f);
                totalMB = static_cast<float>(total) / (1024.0f * 1024.0f);
                return true;
            }

            return false;
        }

        // Helper to draw a graph with label
        void drawGraph(const char* label, const char* id, RingBuffer<float, 300>& history, float minVal, float maxVal,
                       const char* overlayFmt, ImVec4 color = UI::GraphColor(0))
        {
            ImGui::Text("%s", label);

            // Get data for plotting
            float data[300];
            history.copyTo(data);

            ImGui::PushStyleColor(ImGuiCol_PlotLines, color);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, UI::ContainerLow());

            char overlay[64];
            snprintf(overlay, sizeof(overlay), overlayFmt, history.size() > 0 ? history.get(history.size() - 1) : 0.0f);

            ImGui::PlotLines(id, data, static_cast<int>(history.size()), 0, overlay,
                            minVal, maxVal, ImVec2(-1, 60));

            ImGui::PopStyleColor(2);

            // Stats below graph
            if (history.size() > 0)
            {
                ImGui::TextDisabled("Min: %.1f  Avg: %.1f  Max: %.1f",
                    history.min(), history.avg(), history.max());
            }
        }
    }

    void ImGuiOverlay::renderDiagnosticsView()
    {
        // Initialize on first call
        static bool initialized = false;
        // re-resolve when the device changes, not just once: the DRM scan is
        // keyed on the PCI slot the game is actually running on
        static std::string resolvedSlot;
        if (!initialized || resolvedSlot != deviceInfo.gpuPciSlot)
        {
            if (initialized)
                Logger::info("Diagnostics: GPU changed to " + deviceInfo.gpuName + ", re-resolving");
            gpuInfo = findGpu(deviceInfo);
            resolvedSlot = deviceInfo.gpuPciSlot;
            detectedGameName = ConfigSerializer::detectGameName();
            autoDetectedConfig = ConfigSerializer::autoDetectConfig();
            lastFrameTime = std::chrono::steady_clock::now();
            initialized = true;

            if (gpuInfo.vendor != GpuVendor::Unknown)
                Logger::info("Diagnostics: Found " + gpuInfo.vendorName + " GPU at " + gpuInfo.drmCardPath);
            else
                Logger::info("Diagnostics: No supported GPU found");
        }

        // this view only runs while its tab is open. catch the open edge so a
        // stale timestamp from a previous visit doesn't spike the first frame.
        static bool wasOpen = false;
        const bool justOpened = !wasOpen;
        wasOpen = true;

        // Calculate frame time
        auto now = std::chrono::steady_clock::now();
        float frameTimeMs = std::chrono::duration<float, std::milli>(now - lastFrameTime).count();
        lastFrameTime = now;
        if (justOpened)
            frameTimeMs = 0.0f;  // no meaningful delta across a tab switch

        // Only record if reasonable (avoid spikes from tab switching)
        if (frameTimeMs > 0.1f && frameTimeMs < 500.0f)
            frameTimeHistory.push(frameTimeMs);

        // Sample GPU stats at a fixed wall-clock interval (~200ms) so overhead
        // stays constant regardless of frame rate (not "every 10 frames" which
        // would over-poll at high FPS and under-poll at low FPS).
        static auto lastGpuSampleTime = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastGpuSampleTime).count() >= 200)
        {
            lastGpuSampleTime = now;

            float gpuUsage = getGpuUsage();
            if (gpuUsage >= 0)
                gpuUsageHistory.push(gpuUsage);

            float vramUsed, vramTotal;
            if (getVramUsage(vramUsed, vramTotal))
                vramUsageHistory.push((vramUsed / vramTotal) * 100.0f);

            float gttUsed, gttTotal;
            if (getGttUsage(gttUsed, gttTotal))
                gttUsageHistory.push((gttUsed / gttTotal) * 100.0f);
        }

        ImGui::BeginChild("DiagnosticsContent", ImVec2(0, 0), false);

        // FPS counts frames per wall-clock second. 1000/avg() over the 300-frame
        // history lagged badly and reported the past, not the present. carry the
        // leftover into the next window so the seconds don't drift.
        static float dispFps = 0.0f;
        static float dispFps1Low = 0.0f;
        static float dispGpuUsage = -1.0f;
        static float dispVramUsed = 0.0f, dispVramTotal = 0.0f;
        static float dispGttUsed = 0.0f, dispGttTotal = 0.0f;
        static bool dispHasVram = false, dispHasGtt = false;
        static int   fpsFrameCount = 0;
        static std::chrono::steady_clock::time_point fpsWindowStart = now;
        if (justOpened)
        {
            // fresh window on open, so the first second is real.
            fpsWindowStart = now;
            fpsFrameCount = 0;
        }
        fpsFrameCount++;
        const double windowSeconds = std::chrono::duration<double>(now - fpsWindowStart).count();
        if (windowSeconds >= 1.0)
        {
            dispFps = static_cast<float>(fpsFrameCount) / static_cast<float>(windowSeconds);
            dispFps1Low = frameTimeHistory.max() > 0.0f ? 1000.0f / frameTimeHistory.max() : 0.0f;
            dispGpuUsage = gpuInfo.hasGpuUsage ? getGpuUsage() : -1.0f;
            dispHasVram = getVramUsage(dispVramUsed, dispVramTotal);
            dispHasGtt = getGttUsage(dispGttUsed, dispGttTotal);
            // keep the leftover rather than snapping to exactly 1.0s, which under-counts.
            fpsWindowStart += std::chrono::milliseconds(static_cast<int64_t>(windowSeconds * 1000.0));
            fpsFrameCount = 0;
        }
        const float fps = dispFps;
        const float fps1Low = dispFps1Low;

        const float brandSize = std::min(512.0f, ImGui::GetContentRegionAvail().x * 0.288f);
        renderCenteredBrandIcon(brandSize);
        ImGui::Spacing();

        // --- Performance ---
        ImGui::M3CardBegin("diag_perf", "Performance", Icon::SpeedUtf8);
        {
            ImFont* heroFont = ImGuiM3FontBold();
            if (!heroFont)
                heroFont = ImGui::GetIO().Fonts->Fonts[0];
            ImGui::PushFont(heroFont, ImGui::GetFontSize() * 2.4f);
            ImGui::TextColored(UI::Success(), "%.0f FPS", fps);
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextDisabled("(1%% low: %.0f)", fps1Low);
        }
        ImGui::Spacing();
        drawGraph("Frame Time", "##frametime", frameTimeHistory, 0.0f, 50.0f, "%.1f ms", UI::GraphColor(0));
        ImGui::M3CardEnd();

        // --- GPU ---
        ImGui::Spacing();
        ImGui::M3CardBegin("diag_gpu", "GPU", Icon::MemoryUtf8);
        {
            // report what the game is really running on, not a vendor guessed
            // from DRM card order
            if (!deviceInfo.gpuName.empty())
            {
                ImFont* bold = ImGuiM3FontBold();
                if (bold)
                    ImGui::PushFont(bold, ImGui::GetFontSize());
                ImGui::TextUnformatted(deviceInfo.gpuName.c_str());
                if (bold)
                    ImGui::PopFont();

                std::string info;
                if (deviceInfo.gpuApiVersion)
                    info = "Vulkan " + std::to_string(VK_API_VERSION_MAJOR(deviceInfo.gpuApiVersion)) + "." +
                           std::to_string(VK_API_VERSION_MINOR(deviceInfo.gpuApiVersion)) + "." +
                           std::to_string(VK_API_VERSION_PATCH(deviceInfo.gpuApiVersion));
                if (!deviceInfo.gpuDriverInfo.empty())
                    info += info.empty() ? deviceInfo.gpuDriverInfo : "  ·  " + deviceInfo.gpuDriverInfo;
                if (!info.empty())
                    ImGui::TextDisabled("%s", info.c_str());
            }
            else if (gpuInfo.vendor != GpuVendor::Unknown)
            {
                ImGui::TextDisabled("%s", gpuInfo.vendorName.c_str());
            }
            ImGui::Spacing();

            if (gpuInfo.vendor != GpuVendor::Unknown && gpuInfo.hasGpuUsage)
            {
                if (dispGpuUsage >= 0)
                {
                    const char* usageLabel = (gpuInfo.vendor == GpuVendor::Intel) ? "GPU Frequency" : "GPU Usage";
                    drawGraph(usageLabel, "##gpuusage", gpuUsageHistory, 0.0f, 100.0f, "%.0f%%", UI::GraphColor(1));
                    if (gpuInfo.vendor == GpuVendor::Intel)
                        ImGui::TextDisabled("(estimated from frequency ratio)");
                    ImGui::Spacing();
                }
            }

            if (dispHasVram)
            {
                ImGui::Text("VRAM: %.0f / %.0f MB", dispVramUsed, dispVramTotal);
                ImGui::ProgressBar(dispVramUsed / dispVramTotal, ImVec2(-1, 0));
                ImGui::Spacing();
            }

            if (dispHasGtt)
            {
                ImGui::Text("GTT (shared): %.0f / %.0f MB", dispGttUsed, dispGttTotal);
                ImGui::ProgressBar(dispGttUsed / dispGttTotal, ImVec2(-1, 0));
                ImGui::Spacing();
                drawGraph("Memory Usage", "##gttusage", gttUsageHistory, 0.0f, 100.0f, "%.0f%%", UI::GraphColor(2));
            }
            else if (dispHasVram)
            {
                drawGraph("VRAM Usage", "##vramusage", vramUsageHistory, 0.0f, 100.0f, "%.0f%%", UI::GraphColor(2));
            }
        }
        if (deviceInfo.gpuName.empty())
        {
            ImGui::TextDisabled("GPU stats not available.");
            ImGui::TextDisabled("No AMD/Intel/NVIDIA GPU detected via sysfs.");
        }
        ImGui::M3CardEnd();

        // --- Game ---
        ImGui::Spacing();
        ImGui::M3CardBegin("diag_game", "Game", Icon::PlayArrowUtf8);
        if (!detectedGameName.empty())
        {
            ImGui::Text("Executable: %s", detectedGameName.c_str());
            if (!autoDetectedConfig.empty())
                ImGui::TextColored(UI::Success(), "%s  Config: %s.conf (auto-detected)", Icon::CheckCircleUtf8, autoDetectedConfig.c_str());
            else
                ImGui::TextDisabled("No per-game config found.");
        }
        else
        {
            ImGui::TextDisabled("Could not detect the game executable.");
        }
        ImGui::M3CardEnd();

        // --- Credits ---
        ImGui::Spacing();
        ImGui::M3CardBegin("diag_credits", "Credits", Icon::InfoUtf8);
        auto credit = [](const char* what, const char* handle, const char* url) {
            ImGui::TextDisabled("%s", what);
            ImGui::SameLine();
            ImGui::TextLinkOpenURL(handle, url);
        };
        credit("VKIntox maintained by", "@buwryme", "https://github.com/buwryme");
        credit("vkShade by", "@slobodaapl", "https://github.com/slobodaapl");
        credit("vkBasalt by", "@DadSchoorse", "https://github.com/DadSchoorse/vkBasalt");
        credit("Overlay fork by", "@Boux", "https://github.com/Boux/vkBasalt_overlay");
        credit("Wayland overlay by", "@Daaboulex", "https://github.com/Daaboulex/vkBasalt_overlay_wayland");
        credit("ReShade FX support by", "@crosire", "https://github.com/crosire/reshade");
        credit("Dear ImGui by", "@ocornut", "https://github.com/ocornut/imgui");
        ImGui::M3CardEnd();

        // --- Build footer ---
        ImGui::Spacing();
        static const std::string runtimeVersion = [] {
            std::ifstream versionFile(ConfigSerializer::getBaseConfigDir() + "/version");
            std::string version;
            if (versionFile.is_open() && std::getline(versionFile, version) && !version.empty())
            {
                if (!version.empty() && version.back() == '\r')
                    version.pop_back();
                if (!version.empty())
                    return version;
            }
            return std::string("unknown");
        }();
        ImGui::TextDisabled("VKIntox version %s", runtimeVersion.c_str());
        ImGui::TextDisabled("Report issues:");
        ImGui::SameLine();
        ImGui::TextLinkOpenURL("github.com/buwryme/VKIntox/issues", "https://github.com/buwryme/VKIntox/issues");

        ImGui::EndChild();
    }

} // namespace VKIntox
