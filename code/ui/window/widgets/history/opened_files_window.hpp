#pragma once

#include "Image_viewer_panel.hpp"
#include "window_state_toml.hpp"


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
    void ApplyLayout(const WindowStateToml &state);
    void ExportLayout(WindowStateToml *state) const;
    void sync_history(const std::vector<WindowStateToml::ImageHistoryEntry> &history);

    /// Register a callback invoked when the user right-clicks an entry and selects
    /// "Remove from History".  The argument is the source (path or URL) to erase.
    void SetEraseHistoryEntryCallback(std::function<void(const std::string &)> cb);

    /// Register a callback invoked when the user chooses "Restart Preview Thread".
    void SetRestartPreviewCallback(std::function<void()> cb);

    /// Register a callback invoked when the user clicks "Rescan TOML".
    void SetRescanTomlCallback(std::function<void()> cb);

    /// Register callbacks used by the Opened Files menu bar shortcuts.
    void SetMenuShortcutsCallbacks(std::function<void()> on_open_image,
                                   std::function<void()> on_open_online,
                                   std::function<void()> on_fix_videos,
                                   std::function<bool()> is_startup_videos_fixed = nullptr);

    /// Register a callback invoked when the user selects "Quit App".
    void SetQuitCallback(std::function<void()> cb);

    std::optional<WindowStateToml::ImageHistoryEntry> draw(const ImageViewerPanel &viewer,
                                                            HistoryPreview &preview,
                                                            int *focus_id,
                                                            VideoContextMenu *video_ctx = nullptr);

    bool IsOpen;

private:
    ImGuiTextFilter m_filter;
    std::vector<WindowStateToml::ImageHistoryEntry> m_history;
    std::function<void(const std::string &)> m_on_erase_entry;
    std::function<void()>                    m_on_restart_preview;
    std::function<void()>                    m_on_rescan_toml;
    std::function<void()>                    m_on_open_image;
    std::function<void()>                    m_on_open_online;
    std::function<void()>                    m_on_fix_videos;
    std::function<bool()>                    m_is_startup_videos_fixed;
    std::function<void()>                    m_on_quit;
};
