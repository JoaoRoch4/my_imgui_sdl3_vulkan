#pragma once

#include "pch.hpp"

struct SDL_Window;

/// Right-click context menu for a file-browser item (non-directory files).
///
/// Actions:
///   - "Open"                 — reported via Result::open / open_path so the
///                              caller can queue the file for playback/viewing.
///   - "Copy Path"            — copies the absolute path to the clipboard.
///   - "Delete"               — shows a confirmation modal then deletes the
///                              file from disk; reported via Result::deleted.
///   - "Open Containing Folder" — launches the parent directory in the
///                              system file manager (xdg-open on Linux).
///
/// Typical usage inside the SetContextMenuCallback:
///
///   file_explorer.SetContextMenuCallback([this](const std::filesystem::path &p) {
///       auto res = m_fb_context_menu->draw(p);
///       if (res.open)    queue_path(res.open_path.string());
///       if (res.deleted) { /* refresh, etc. */ }
///   });
class FileBrowserContextMenu {
public:
    /// Result reported back to the caller after draw().
    struct Result {
        bool                  open    = false;
        std::filesystem::path open_path;

        bool                  deleted = false;
        std::filesystem::path deleted_path;
    };

    FileBrowserContextMenu();

    FileBrowserContextMenu(const FileBrowserContextMenu &) = delete;
    FileBrowserContextMenu &operator=(const FileBrowserContextMenu &) = delete;

    /// Store the SDL window (currently unused, reserved for future dialogs).
    void setup(SDL_Window *window);

    /// Call this from SetContextMenuCallback for each file path.
    /// Internally calls BeginPopupContextItem so it must be called immediately
    /// after the Selectable that represents the file item.
    ///
    /// @return  Result describing any action chosen by the user.
    Result draw(const std::filesystem::path &path);

    /// Optional callback invoked at the bottom of every popup (before it
    /// closes), allowing callers to append extra menu items.
    /// Call once after setup(); pass nullptr to clear.
    void SetExtraItemsCallback(
        std::function<void(const std::filesystem::path &)> cb);

    /// Execute any deferred operations (confirmation modals).
    /// Call once per ImGui frame from the main render loop.
    void process_pending();

private:
    /// Draw the inner menu items (no BeginPopup/EndPopup wrapper).
    Result draw_menu_items(const std::filesystem::path &path);

    SDL_Window *m_window = nullptr;

    // State for the delete-confirmation modal.
    bool                  m_want_open_modal    = false;
    std::filesystem::path m_pending_delete_path;

    std::function<void(const std::filesystem::path &)> m_extra_items_cb;
};
