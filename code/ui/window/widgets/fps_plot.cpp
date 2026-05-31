#include "pch.hpp"

#include "fps_plot.hpp"

FpsPlot::FpsPlot()
    : IsOpen{true}, m_samples{}, m_head{0}, m_count{0}, m_total_samples{0},
      m_last_sample_time{} {}

void FpsPlot::add_sample(float fps) {
  using namespace std::chrono_literals;

  const auto now = std::chrono::steady_clock::now();
  if (m_last_sample_time.time_since_epoch().count() != 0 &&
      (now - m_last_sample_time) < 1s) {
    return;
  }

  m_last_sample_time = now;

  m_samples[static_cast<size_t>(m_head)] = fps;
  m_head = (m_head + 1) % kHistorySize;
  if (m_count < kHistorySize)
    ++m_count;
  ++m_total_samples;
}

void FpsPlot::draw(double uptime_seconds) {
  if (!IsOpen)
    return;

  if (!ImGui::Begin("FPS Plot", &IsOpen)) {
    ImGui::End();
    return;
  }

  ImGui::Text("Current FPS: %.1f", ImGui::GetIO().Framerate);
  const int total_seconds = static_cast<int>(uptime_seconds);
  const int hours = total_seconds / 3600;
  const int minutes = (total_seconds % 3600) / 60;
  const int seconds = total_seconds % 60;
  ImGui::Text("Uptime: %02d:%02d:%02d", hours, minutes, seconds);

  if (m_count > 1) {
    std::array<float, kHistorySize> x_vals{};
    std::array<float, kHistorySize> y_vals{};

    const int start = (m_head - m_count + kHistorySize) % kHistorySize;
    for (int i = 0; i < m_count; ++i) {
      const int idx = (start + i) % kHistorySize;
      x_vals[static_cast<size_t>(i)] =
          static_cast<float>(m_total_samples - m_count + i);
      y_vals[static_cast<size_t>(i)] = m_samples[static_cast<size_t>(idx)];
    }

    const float fps_max =
        *std::max_element(y_vals.begin(), y_vals.begin() + m_count);
    const float y_limit = std::max(60.0f, fps_max + 10.0f);
    const float x_min = x_vals[0];
    const float x_max = x_vals[static_cast<size_t>(m_count - 1)] + 1.0f;

    if (ImPlot::BeginPlot("##fps_history", ImVec2(-1.0f, 220.0f))) {
      ImPlot::SetupAxes("Frame", "FPS", ImPlotAxisFlags_NoTickLabels,
                        ImPlotAxisFlags_None);
      ImPlot::SetupAxisLimits(ImAxis_X1, x_min, x_max, ImGuiCond_Always);
      ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, y_limit, ImGuiCond_Always);
      ImPlot::PlotLine("FPS", x_vals.data(), y_vals.data(), m_count);
      ImPlot::EndPlot();
    }
  } else {
    ImGui::TextDisabled("Collecting samples...");
  }

  ImGui::End();
}
