#pragma once

#include "window_state_toml.hpp"

#include <filesystem>
#include <string>

struct SDL_Window;

/// Right-click context menu for a video history entry.
///
/// Provides two actions:
///   - "Remove from History"  — reported via Result::erase.
///   - "Save Video As…"       — opens a native save-file dialog and copies
///                              the cached or local video file to the chosen
///                              destination.  The copy is deferred; call
///                              process_pending_save() once per frame.
class VideoContextMenu {
public:
    /// Result reported back to the caller after draw_for_item().
    struct Result {
        bool        erase  = false;
        std::string erase_source;
    };

    VideoContextMenu();

    VideoContextMenu(const VideoContextMenu &) = delete;
    VideoContextMenu &operator=(const VideoContextMenu &) = delete;

    /// Store the SDL window used to parent the native save-file dialog.
    void setup(SDL_Window *window);

    /// Attach a context menu popup to the last rendered ImGui item.
    ///
    /// Internally resolves the best source file for "Save Video":
    ///   1. cached_path (preferred — already on disk)
    ///   2. entry.source when kind == "file" and the path exists
    /// "Save Video" is disabled when neither is available.
    ///
    /// @return  Result describing any erase action chosen by the user.
    Result draw_for_item(const WindowStateToml::ImageHistoryEntry &entry);

    /// Variant that attaches to the current ImGui *window* (right-click
    /// anywhere in the window).  Same logic and return value as draw_for_item.
    Result draw_for_window(const WindowStateToml::ImageHistoryEntry &entry,
                           const char *popup_id = "##video_window_ctx");

    /// Draw only the menu items (no BeginPopup / EndPopup).
    /// The caller is responsible for opening and closing the popup.
    /// Use this when combining these items with additional items in one popup.
    Result draw_menu_items(const WindowStateToml::ImageHistoryEntry &entry);

    /// Execute any copy queued by the save-file dialog result.
    /// Call once per ImGui frame from the main loop.
    void process_pending_save();

    static void save_dialog_callback(void *userdata,
                                     const char *const *filelist,
                                     int filter);

private:

    SDL_Window *m_window;

    /// Source path stored when the user picks "Save Video As…".
    std::filesystem::path m_copy_source;

    /// Destination path stored by the dialog callback; empty = nothing pending.
    std::filesystem::path m_copy_dest;
};
