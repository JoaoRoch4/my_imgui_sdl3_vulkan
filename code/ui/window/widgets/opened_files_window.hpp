#pragma once

#include "Image_viewer_panel.hpp"
#include "window_state_toml.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class HistoryPreview;
class VideoContextMenu;

class OpenedFilesWindow {
public:
    OpenedFilesWindow();
    ~OpenedFilesWindow() = default;

    OpenedFilesWindow(const OpenedFilesWindow &) = delete;
    OpenedFilesWindow &operator=(const OpenedFilesWindow &) = delete;

    bool load_history_from_toml(const std::filesystem::path &file_path);
    void apply_history(const WindowStateToml &state);
    void sync_history(const std::vector<WindowStateToml::ImageHistoryEntry> &history);

    /// Register a callback invoked when the user right-clicks an entry and selects
    /// "Remove from History".  The argument is the source (path or URL) to erase.
    void SetEraseHistoryEntryCallback(std::function<void(const std::string &)> cb);

    std::optional<WindowStateToml::ImageHistoryEntry> draw(const ImageViewerPanel &viewer,
                                                            HistoryPreview &preview,
                                                            int *focus_id,
                                                            VideoContextMenu *video_ctx = nullptr);

    bool IsOpen;

private:
    ImGuiTextFilter m_filter;
    std::vector<WindowStateToml::ImageHistoryEntry> m_history;
    std::function<void(const std::string &)> m_on_erase_entry;
};
