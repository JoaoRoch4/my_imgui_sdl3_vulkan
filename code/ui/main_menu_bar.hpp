#pragma once

#include "Image_viewer_panel.hpp"
#include "window_state_toml.hpp"

#include <array>
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

    /// Save image history into persisted state.
    void ExportHistory(WindowStateToml *state) const;

    /// Unload all GPU resources. Must be called before ImGui_ImplVulkan_Shutdown.
    void Shutdown();

    bool request_quit; ///< Set to true when File > Quit is clicked.

private:
    /// SDL file-dialog callback — fires on the main thread with the chosen paths.
    static void file_dialog_callback(void *userdata,
                                     const char *const *filelist,
                                     int filter);

    /// Emit a timestamp string "YYYY-MM-DDTHH:MM:SS" into dst.
    static void current_timestamp(std::array<char, 20> &dst);

    /// Push one entry onto the front of m_history with the given source and kind.
    void push_history(const std::string &source, const std::string &kind);
    void handle_interactions(struct ImageEntry &entry, ImVec2 pos, ImVec2 size, float base_scale);

    StyleEditor *m_style_editor;
    SDL_Window *m_window;
    vulkan_context *m_vk;
    bool *m_show_demo_window;
    bool *m_show_another_window;

    ImageViewerPanel m_viewer; ///< Owns all image windows.

    std::vector<WindowStateToml::ImageHistoryEntry> m_history; ///< Recently opened items.

    bool m_has_pending_path;                  ///< Set by file_dialog_callback.
    std::vector<std::string> m_pending_paths; ///< Paths queued from the file dialog.
    std::vector<std::string> m_pending_urls;  ///< URLs queued to download.

    bool m_show_url_popup;           ///< Triggers OpenPopup on the next frame.
    std::array<char, 512> m_url_buf; ///< Input buffer for the URL popup.
};