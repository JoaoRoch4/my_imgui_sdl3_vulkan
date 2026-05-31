#pragma once

#include "window_state_toml.hpp"

#include <functional>
#include <string>
#include <vector>

class RecentHistoryMenu {
public:
    struct Callbacks {
        std::function<void(WindowStateToml::ImageHistoryEntry &)> on_open;
        std::function<void(WindowStateToml::ImageHistoryEntry &)> on_hover;
        std::function<bool(WindowStateToml::ImageHistoryEntry &)> on_after_item;
    };

    struct DrawResult {
        int  shown;
        bool stopped;
    };

    static std::string build_label(const WindowStateToml::ImageHistoryEntry &entry);

    static DrawResult draw_entries(
        const std::vector<WindowStateToml::ImageHistoryEntry> &history,
        const Callbacks &callbacks,
        int max_items = 20);
};