#pragma once

#include "pch.hpp"

#include "window_state_toml.hpp"


struct SDL_Window;

/// Right-click context menu for an image history entry.
///
/// Provides two actions:
///   - "Remove from History"  — reported via Result::erase.
///   - "Save Image As…"       — opens a native save-file dialog and copies
///                              the cached or local image file to the chosen
///                              destination.  The copy is deferred; call
///                              process_pending_save() once per frame.
class ImageContextMenu {
public:
    /// Result reported back to the caller after draw_for_item().
    struct Result {
        bool        erase         = false;
        std::string erase_source;
    };

    ImageContextMenu();

    ImageContextMenu(const ImageContextMenu &) = delete;
    ImageContextMenu &operator=(const ImageContextMenu &) = delete;

    /// Store the SDL window used to parent the native save-file dialog.
    void setup(SDL_Window *window);

    /// Register a callback invoked after a successful save copy.
    void set_on_save_success(
        std::function<void(const std::string &, const std::filesystem::path &)> cb);

    /// Attach a context menu popup to the last rendered ImGui item.
    ///
    /// Internally resolves the best source file for "Save Image":
    ///   1. cached_path (preferred — already on disk)
    ///   2. entry.source when kind == "file" and the path exists
    /// "Save Image" is disabled when neither is available.
    ///
    /// @return  Result describing any erase action chosen by the user.
    Result draw_for_item(const WindowStateToml::ImageHistoryEntry &entry);

    /// Variant that attaches to the current ImGui *window* (right-click
    /// anywhere in the window).  Same logic and return value as draw_for_item.
    Result draw_for_window(const WindowStateToml::ImageHistoryEntry &entry,
                           const char *popup_id = "##image_window_ctx");

    /// Draw only the menu items (no BeginPopup / EndPopup).
    /// The caller is responsible for opening and closing the popup.
    Result draw_menu_items(const WindowStateToml::ImageHistoryEntry &entry);

    /// Execute any copy queued by the save-file dialog result.
    /// Call once per ImGui frame from the main loop.
    void process_pending_save();

    static void save_dialog_callback(void *userdata,
                                     const char *const *filelist,
                                     int filter);

private:
    SDL_Window *m_window;

    /// Source path stored when the user picks "Save Image As…".
    std::filesystem::path m_copy_source;

    /// Logical history source associated with the pending save operation.
    std::string m_copy_history_source;

    /// Destination path stored by the dialog callback; empty = nothing pending.
    std::filesystem::path m_copy_dest;

    std::function<void(const std::string &, const std::filesystem::path &)> m_on_save_success;
};
