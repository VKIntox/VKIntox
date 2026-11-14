// frame timing shared by the fps pill and the Diagnostics view
#pragma once

namespace VKIntox::FpsMetrics
{
    // once per overlay frame; the first call only sets the baseline
    void sample();

    // fps over the trailing second; lows read 0 before the first one-second tick
    float fps();
    float low1Percent();
    float low01Percent();

    constexpr int ringCapacity = 300;
    // contents, oldest first
    int copyFrameTimes(float* dst);
}
