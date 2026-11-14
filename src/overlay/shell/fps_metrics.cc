#include "overlay/shell/fps_metrics.hh"

#include <algorithm>
#include <chrono>

namespace VKIntox::FpsMetrics
{
    namespace
    {
        float g_ring[ringCapacity] = {};
        int g_ringCount = 0;
        int g_ringNext = 0;

        std::chrono::steady_clock::time_point g_lastFrame = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point g_windowStart = g_lastFrame;
        int g_windowFrames = 0;
        float g_fps = 0.0f;
        float g_low1 = 0.0f;
        float g_low01 = 0.0f;

        float percentile(float p)
        {
            if (g_ringCount == 0)
                return 0.0f;
            float sorted[ringCapacity];
            std::copy(g_ring, g_ring + g_ringCount, sorted);
            std::sort(sorted, sorted + g_ringCount);
            const int last = g_ringCount - 1;
            const int idx = std::clamp(static_cast<int>(last * p + 0.5f), 0, last);
            return sorted[idx];
        }
    }

    void sample()
    {
        const auto now = std::chrono::steady_clock::now();
        const float frameTimeMs = std::chrono::duration<float, std::milli>(now - g_lastFrame).count();
        g_lastFrame = now;
        // stalls past half a second are the game loading, not frame pacing
        if (frameTimeMs > 0.1f && frameTimeMs < 500.0f)
        {
            g_ring[g_ringNext] = frameTimeMs;
            g_ringNext = (g_ringNext + 1) % ringCapacity;
            if (g_ringCount < ringCapacity)
                g_ringCount++;
        }

        g_windowFrames++;
        const double windowSeconds = std::chrono::duration<double>(now - g_windowStart).count();
        if (windowSeconds >= 1.0)
        {
            g_fps = static_cast<float>(g_windowFrames) / static_cast<float>(windowSeconds);
            const float p99 = percentile(0.99f);
            const float p999 = percentile(0.999f);
            g_low1 = p99 > 0.0f ? 1000.0f / p99 : 0.0f;
            g_low01 = p999 > 0.0f ? 1000.0f / p999 : 0.0f;
            // keep the leftover rather than snapping to exactly 1.0s, which under-counts
            g_windowStart += std::chrono::milliseconds(static_cast<int64_t>(windowSeconds * 1000.0));
            g_windowFrames = 0;
        }
    }

    float fps() { return g_fps; }
    float low1Percent() { return g_low1; }
    float low01Percent() { return g_low01; }

    int copyFrameTimes(float* dst)
    {
        for (int i = 0; i < g_ringCount; i++)
            dst[i] = g_ring[(g_ringNext + ringCapacity - g_ringCount + i) % ringCapacity];
        return g_ringCount;
    }
}
