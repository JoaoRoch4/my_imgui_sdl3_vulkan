#pragma once

#include "Image_viewer_panel.hpp"
#include "bulk_image_open_queue.hpp"
#include "config_runtime.hpp"
#include "history_preview.hpp"
#include "open_image_dialogs.hpp"
#include "opened_files_window.hpp"
#include "video_player.hpp"
#include "window_state_toml.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

struct SDL_Window;
class StyleEditor;

/**
 * Owns the application main menu bar.
 *
 * Responsibilities:
 *   File > Open Image   — native OS file dialog via SDL3.
 *   File > Open Online  — URL input popup; downloads and displays the image.
 *   File > Recent       — persisted history of opened files and URLs.
 *   File > Quit         — sets request_quit = true.
 *   View                — delegates image toggle items to ImageViewerPanel.
 *
 * Image rendering is fully delegated to the ImageViewerPanel member.
 * MainMenuBar only handles file loading, URL downloading, and history.
 */
class MainMenuBar {
public:
    MainMenuBar();
    ~MainMenuBar() = default;

    MainMenuBar(const MainMenuBar &) = delete;
    MainMenuBar &operator=(const MainMenuBar &) = delete;

    void Setup(StyleEditor *style_editor,
               SDL_Window *window,
               vulkan_context *vk,
               bool *show_demo_window,
               bool *show_another_window);

    /// Call once per frame between NewFrame() and Render().
    void Build();

    /// Restore image history from persisted state.
    void ApplyHistory(const WindowStateToml &state);

    /// Restore runtime config values from persisted state.
    void ApplyRuntimeConfig(const WindowStateToml &state);

    /// Explicitly load opened-files history from TOML at startup.
    bool LoadOpenedFilesHistoryFromToml(const std::filesystem::path &file_path);

    /// Save image history into persisted state.
    void ExportHistory(WindowStateToml *state) const;

    /// Save runtime config values into persisted state.
    void ExportRuntimeConfig(WindowStateToml *state) const;

    /// Unload all GPU resources. Must be called before ImGui_ImplVulkan_Shutdown.
    void Shutdown();

    bool request_quit; ///< Set to true when File > Quit is clicked.

private:
    /// Emit a timestamp string "YYYY-MM-DDTHH:MM:SS" into dst.
    static void current_timestamp(std::array<char, 20> &dst);

    /// Push one entry onto the front of m_history with the given source and kind.
    void push_history(const std::string &source, const std::string &kind);

    StyleEditor *m_style_editor;
    SDL_Window *m_window;
    vulkan_context *m_vk;
    bool *m_show_demo_window;
    bool *m_show_another_window;

    ImageViewerPanel m_viewer; ///< Owns all image windows.

    std::vector<WindowStateToml::ImageHistoryEntry> m_history; ///< Recently opened items.

    OpenImageDialogs m_open_image_dialogs;
    BulkImageOpenQueue m_bulk_image_open;

    VideoPlayer m_video_player;

    ConfigRuntime m_config_runtime;

    HistoryPreview m_history_preview;
    OpenedFilesWindow m_opened_files_window;
};