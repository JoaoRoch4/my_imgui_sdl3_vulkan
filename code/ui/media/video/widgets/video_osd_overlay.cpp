#include "video_osd_overlay.hpp"

#include <algorithm>

VideoOsdOverlay::VideoOsdOverlay()
    : m_message{}
    , m_expires_at{}
    , m_default_duration{std::chrono::milliseconds(1200)}
{
}

void VideoOsdOverlay::show(const std::string &message)
{
    show(message, m_default_duration);
}

void VideoOsdOverlay::show(const std::string &message, std::chrono::milliseconds duration)
{
    m_message = message;
    m_expires_at = std::chrono::steady_clock::now() + duration;
}

void VideoOsdOverlay::clear()
{
    m_message.clear();
    m_expires_at = std::chrono::steady_clock::time_point{};
}

bool VideoOsdOverlay::visible() const
{
    return !m_message.empty() && std::chrono::steady_clock::now() < m_expires_at;
}

void VideoOsdOverlay::draw(ImVec2 canvas_pos, ImVec2 canvas_size) const
{
    if (!visible())
        return;

    const auto now = std::chrono::steady_clock::now();
    const auto fade_window = std::chrono::milliseconds(250);
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(m_expires_at - now);

    float alpha = 1.0f;
    if (remaining < fade_window) {
        alpha = std::clamp(static_cast<float>(remaining.count()) /
                               static_cast<float>(fade_window.count()),
                           0.0f,
                           1.0f);
    }

    const ImVec2 text_size = ImGui::CalcTextSize(m_message.c_str());
    const ImVec2 padding{12.0f, 8.0f};
    const ImVec2 box_size{text_size.x + padding.x * 2.0f, text_size.y + padding.y * 2.0f};
    const ImVec2 box_min{
        canvas_pos.x + (canvas_size.x - box_size.x) * 0.5f,
        canvas_pos.y + std::max(canvas_size.y - box_size.y - 18.0f, 8.0f),
    };
    const ImVec2 box_max{box_min.x + box_size.x, box_min.y + box_size.y};

    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(box_min,
                             box_max,
                             ImGui::GetColorU32(ImVec4(0.04f, 0.04f, 0.04f, 0.78f * alpha)),
                             6.0f);
    draw_list->AddRect(box_min,
                       box_max,
                       ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.18f * alpha)),
                       6.0f);
    draw_list->AddText(ImVec2(box_min.x + padding.x, box_min.y + padding.y),
                       ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha)),
                       m_message.c_str());
}