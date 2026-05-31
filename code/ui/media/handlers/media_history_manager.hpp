#pragma once

#include "window_state_toml.hpp"


#include <array>
#include <filesystem>
#include <string>
#include <vector>

class OpenedFilesWindow;

/**
 * Owns the in-memory recently-opened-media history and its TOML persistence.
 *
 * Extracted from MainMenuBar so that history operations (push, erase, clear,
 * apply from disk, export to disk) have a single home.  MainMenuBar holds one
 * instance of this class and calls through it instead of managing the raw
 * vector and persistence helpers itself.
 */
class MediaHistoryManager {
public:
    MediaHistoryManager();
    ~MediaHistoryManager() = default;

    MediaHistoryManager(const MediaHistoryManager &) = delete;
    MediaHistoryManager &operator=(const MediaHistoryManager &) = delete;

    void set_state_path(const std::filesystem::path &path);

    void push(const std::string &source,
              const std::string &kind,
              const std::string &title,
              OpenedFilesWindow &files_window);

    void replace_with_saved_file(const std::string &source,
                                 const std::filesystem::path &saved_path,
                                 OpenedFilesWindow &files_window);

    void set_hwdec_enabled(const std::string &source,
                           bool enabled,
                           OpenedFilesWindow &files_window,
                           int resume_position_seconds = -1);

    void set_playback_mode(const std::string &source,
                           int mode,
                           OpenedFilesWindow &files_window,
                           int resume_position_seconds = -1);

    void erase(const std::string &source, OpenedFilesWindow &files_window);
    void clear(OpenedFilesWindow &files_window);

    void apply(const WindowStateToml &state, OpenedFilesWindow &files_window);
    void export_to(WindowStateToml *state) const;
    void persist() const;

    [[nodiscard]] const std::vector<WindowStateToml::ImageHistoryEntry> &entries() const;
    [[nodiscard]] std::vector<WindowStateToml::ImageHistoryEntry> &entries();

private:
    static void current_timestamp(std::array<char, 20> &dst);

    std::vector<WindowStateToml::ImageHistoryEntry> m_entries; ///< In-memory list, newest first.
    std::filesystem::path m_state_path;                        ///< TOML file used by persist().
};