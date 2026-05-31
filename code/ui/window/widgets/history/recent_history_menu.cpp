#include "recent_history_menu.hpp"

#include "imgui.h"

#include <filesystem>

std::string RecentHistoryMenu::build_label(const WindowStateToml::ImageHistoryEntry &entry)
{
    std::string label;
    if (entry.kind == "file") {
        label = "[file]  ";
        label += std::filesystem::path(entry.source).filename().string();
        return label;
    }

    label = "[url]   ";
    const std::string shown_title = !entry.title.empty() ? entry.title : entry.source;
    label += shown_title.size() > 60 ? shown_title.substr(0, 57) + "..." : shown_title;
    return label;
}

RecentHistoryMenu::DrawResult RecentHistoryMenu::draw_entries(
    const std::vector<WindowStateToml::ImageHistoryEntry> &history,
    const Callbacks &callbacks,
    int max_items)
{
    DrawResult result{0, false};

    for (size_t index = 0; index < history.size() && result.shown < max_items; ++index) {
        WindowStateToml::ImageHistoryEntry entry = history[index];
        ++result.shown;

        const std::string label = build_label(entry);
        if (ImGui::MenuItem(label.c_str()) && callbacks.on_open)
            callbacks.on_open(entry);

        if (ImGui::IsItemHovered() && callbacks.on_hover)
            callbacks.on_hover(entry);

        if (callbacks.on_after_item && callbacks.on_after_item(entry)) {
            result.stopped = true;
            break;
        }
    }

    return result;
}