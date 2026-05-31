#pragma once
#include "pch.hpp"


class FpsPlot {
public:
    FpsPlot();

    void add_sample(float fps);
    void draw(double uptime_seconds);

    bool IsOpen;

private:
    static constexpr int kHistorySize = 240;

    std::array<float, kHistorySize> m_samples;
    int m_head;
    int m_count;
    int m_total_samples;
    std::chrono::steady_clock::time_point m_last_sample_time;
};
