#pragma once

#include "pch.hpp"

struct WindowStateToml;

class ConfigRuntime;
class HistoryPreview;
class MediaHistoryManager;
class OpenedFilesWindow;
class VideoDownloader;

class AppStateCoordinator {
public:
  AppStateCoordinator();

  AppStateCoordinator(const AppStateCoordinator &) = delete;
  AppStateCoordinator &operator=(const AppStateCoordinator &) = delete;

  void setup(MediaHistoryManager *history_mgr, ConfigRuntime *config_runtime,
             HistoryPreview *history_preview, OpenedFilesWindow *files_window,
             VideoDownloader *video_downloader);

  void apply_history(const WindowStateToml &state);
  void apply_runtime_config(const WindowStateToml &state);
  bool
  load_opened_files_history_from_toml(const std::filesystem::path &file_path);

  void set_state_path(const std::filesystem::path &file_path);
  void export_history(WindowStateToml *state) const;
  void export_runtime_config(WindowStateToml *state) const;

  void set_thumb_dir(const std::filesystem::path &dir);
  void set_download_cache_dir(const std::filesystem::path &dir);
  void clear_all_cache_and_state();

  [[nodiscard]] bool
  is_startup_video_fixed(const std::string &video_source) const;
  void set_startup_video(const std::string &video_source, bool should_restore);
  void toggle_startup_video(const std::string &video_source);
  [[nodiscard]] bool are_all_startup_videos_fixed(
      const std::vector<std::string> &video_sources) const;
  void
  set_startup_video_for_sources(const std::vector<std::string> &video_sources,
                                bool should_restore);

  [[nodiscard]] const std::filesystem::path &state_path() const;
  [[nodiscard]] bool take_restore_videos_on_startup_pending();

private:
  MediaHistoryManager *m_history_mgr;
  ConfigRuntime *m_config_runtime;
  HistoryPreview *m_history_preview;
  OpenedFilesWindow *m_files_window;
  VideoDownloader *m_video_downloader;

  std::filesystem::path m_thumb_dir;
  std::filesystem::path m_download_cache_dir;
  std::filesystem::path m_state_path;
  bool m_restore_videos_on_startup_pending;
};