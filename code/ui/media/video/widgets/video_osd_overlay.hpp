#pragma once

#include <chrono>
#include <string>

#include "imgui.h"

class VideoOsdOverlay {
public:
    VideoOsdOverlay();

    void show(const std::string &message);
    void show(const std::string &message, std::chrono::milliseconds duration);
    void clear();

    [[nodiscard]] bool visible() const;
    void draw(ImVec2 canvas_pos, ImVec2 canvas_size) const;

private:
    std::string                           m_message;
    std::chrono::steady_clock::time_point m_expires_at;
    std::chrono::milliseconds             m_default_duration;
};