#pragma once

#include "Image_viewer_panel.hpp"
#include "history_preview.hpp"
#include "window_state_toml.hpp"

#include <filesystem>
#include <optional>
#include <vector>

class OpenedFilesWindow {
public:
    OpenedFilesWindow();
    ~OpenedFilesWindow() = default;

    OpenedFilesWindow(const OpenedFilesWindow &) = delete;
    OpenedFilesWindow &operator=(const OpenedFilesWindow &) = delete;

    bool load_history_from_toml(const std::filesystem::path &file_path);
    void apply_history(const WindowStateToml &state);
    void sync_history(const std::vector<WindowStateToml::ImageHistoryEntry> &history);
    std::optional<WindowStateToml::ImageHistoryEntry> draw(const ImageViewerPanel &viewer,
                                                           HistoryPreview &preview,
                                                           int *focus_id);

    bool IsOpen;

private:
    ImGuiTextFilter m_filter;
    std::vector<WindowStateToml::ImageHistoryEntry> m_history;
};
