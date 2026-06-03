#pragma once

#include "pch.hpp" // NOLINT


#include "config_runtime_ui_context.hpp"
#include "main_menu_bar.hpp"

struct SDL_Window;
class StyleEditor;
struct WindowStateToml;
class vulkan_context;
class AppContext;
class ImageViewerPanel;
class OpenImageDialogs;
class BulkImageOpenQueue;
class VideoPlayer;
class VideoPlayerPlacebo;
class VideoDownloader;
class ConfigRuntime;
class HistoryPreview;
class OpenedFilesWindow;
class VideoContextMenu;
class FileBrowserContextMenu;
class FileThumbnailCache;
class MediaHistoryManager;
class MediaLoadHandler;
class AppStateCoordinator;
class ConsoleCommands;
class VulkanEmojiAtlas;
class MetadataEditor;

/**
 * Top-level UI/subsystem coordinator.
 *
 * AppCoordinator owns every subsystem (through AppContext) and is responsible
 * for:
 *   - Wiring the subsystems together in Setup().
 *   - Driving the per-frame orchestration in Build() (background draining,
 *     media loading, window rendering) and delegating the menu-bar rendering to
 *     MainMenuBar.
 *   - Forwarding history and config between the TOML persistence layer and the
 *     subsystems that need them.
 *
 * MainMenuBar now contains *only* the menu-bar drawing; all coordination lives
 * here.
 */
class AppCoordinator {
public:
  AppCoordinator();
  ~AppCoordinator();

  AppCoordinator(const AppCoordinator &) = delete;
  AppCoordinator &operator=(const AppCoordinator &) = delete;

  void Setup(StyleEditor *style_editor, SDL_Window *window, vulkan_context *vk,
             bool *show_demo_window, bool *show_another_window,
             std::function<void(bool)> on_vsync_changed = nullptr);

  /// Call once per frame between NewFrame() and Render().
  void Build();

  /// Restore image history and synchronise companion UI from persisted state.
  void ApplyHistory(const WindowStateToml &state);

  /// Restore runtime config values from persisted state.
  void ApplyRuntimeConfig(const WindowStateToml &state);

  /// Load opened-files history directly from a TOML file at startup.
  bool LoadOpenedFilesHistoryFromToml(const std::filesystem::path &file_path);

  /// Set the TOML file path forwarded to MediaHistoryManager for persistence.
  void SetStatePath(const std::filesystem::path &file_path);

  /// Serialise image history into state for the main save routine.
  void ExportHistory(WindowStateToml *state);

  /// Serialise runtime config into state for the main save routine.
  void ExportRuntimeConfig(WindowStateToml *state) const;

  /// Set the directory where hover-thumbnail PNGs are written and cached.
  void SetThumbDir(const std::filesystem::path &dir);

  /// Set the directory where background-downloaded video files are stored.
  void SetDownloadCacheDir(const std::filesystem::path &dir);

  /// Forward an SDL event to subsystems that need it (e.g. multimedia keys).
  /// Call from the main event loop before ImGui_ImplSDL3_ProcessEvent.
  void HandleSdlEvent(const SDL_Event &event);

  /// Unload all GPU resources. Must be called before ImGui_ImplVulkan_Shutdown.
  void Shutdown();

  bool request_quit;   ///< Set to true when File > Quit is selected.
  bool request_reopen; ///< Set to true when Runtime Config requests app reopen.

private:
  // ---- Non-owning external dependencies (provided by App) -----------------
  StyleEditor *m_style_editor;
  SDL_Window *m_window;
  vulkan_context *m_vk;
  bool *m_show_demo_window;
  bool *m_show_another_window;

  // ---- Central ownership ---------------------------------------------------
  // AppContext is the sole owner of every subsystem below.  It is declared
  // first so it is destroyed last (members tear down in reverse order).
  std::unique_ptr<AppContext> m_ctx;

  // ---- Non-owning aliases into m_ctx (bound once in the constructor) -------
  // These keep the existing `m_xxx->` call sites unchanged while ownership
  // lives in AppContext.  They never own; do not delete or reset them.
  ImageViewerPanel *m_viewer = nullptr;
  OpenImageDialogs *m_open_image_dialogs = nullptr;
  BulkImageOpenQueue *m_bulk_image_open = nullptr;
  VideoPlayer *m_video_player = nullptr;
  VideoPlayerPlacebo *m_video_player_placebo = nullptr;
  VideoDownloader *m_video_downloader = nullptr;
  ConfigRuntime *m_config_runtime = nullptr;
  ConfigRuntimeUiContext m_config_runtime_ui;
  HistoryPreview *m_history_preview = nullptr;
  OpenedFilesWindow *m_opened_files_window = nullptr;
  VideoContextMenu *m_video_context_menu = nullptr;
  FileBrowserContextMenu *m_fb_context_menu = nullptr;
  FileThumbnailCache *m_thumb_cache = nullptr;
  MetadataEditor *m_metadata_editor = nullptr;
  MediaHistoryManager *m_history_mgr = nullptr;
  MediaLoadHandler *m_load_handler = nullptr;
  AppStateCoordinator *m_app_state = nullptr;
  ConsoleCommands *m_console = nullptr;
  VulkanEmojiAtlas *m_emoji_atlas = nullptr;

  // ---- Menu bar (rendering only) ------------------------------------------
  MainMenuBar m_menu;

  bool m_show_console = false;

  bool m_use_video_player_placebo = false;
};
