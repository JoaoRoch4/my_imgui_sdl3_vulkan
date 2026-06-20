#pragma once

#include "pch.hpp" // NOLINT


#include "config_runtime_ui_context.hpp"
#include "main_menu_bar.hpp"

struct SDL_Window;
class StyleEditor;
struct WindowStateToml;
class vulkan_context;
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
 * Every subsystem is owned by the global MemoryManagement registry (PushGet in
 * App::Alloc(), Release in App::destroy()).  AppCoordinator holds only non-owning
 * observer pointers into that registry, bound in Setup() via GetInstance<T>().
 * It is responsible for:
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

  // The former external dependencies (StyleEditor, the SDL_Window, the
  // vulkan_context and the demo/another show flags) are now pulled directly from
  // the MemoryManagement registry inside Setup() via GetInstance<T>(), so App no
  // longer threads them through. Only the vsync behaviour hook is still injected.
  void Setup(std::function<void(bool)> on_vsync_changed = nullptr);

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

  /// Gate which media types may be loaded this session (CLI --no-video/--no-media).
  /// Forwarded to MediaLoadHandler, the single media-routing choke point.
  void SetMediaPolicy(bool allow_video, bool allow_image);

  /// Push one line into the integrated console (thread-safe; buffered until the
  /// next Draw). Used for startup args feedback before the first frame renders.
  void ConsoleLog(std::string line);

  /// Force the integrated console window visible (e.g. to surface startup args
  /// feedback). Call after ApplyRuntimeConfig so it is not overwritten by TOML.
  void ShowConsole();

  /// Set the directory where hover-thumbnail PNGs are written and cached.
  void SetThumbDir(const std::filesystem::path &dir);

  /// Set the directory where background-downloaded video files are stored.
  void SetDownloadCacheDir(const std::filesystem::path &dir);

  /// Forward an SDL event to subsystems that need it (e.g. multimedia keys).
  /// Call from the main event loop before ImGui_ImplSDL3_ProcessEvent.
  /// Returns true when the event was consumed and must NOT be handed to ImGui
  /// (e.g. arrow keys driving the hover preview, so they don't move the file
  /// browser selection underneath the popup).
  [[nodiscard]] bool HandleSdlEvent(const SDL_Event &event);

  /// Unload all GPU resources. Must be called before ImGui_ImplVulkan_Shutdown.
  void Shutdown();

  bool request_quit;   ///< Set to true when File > Quit is selected.
  bool request_reopen; ///< Set to true when Runtime Config requests app reopen.

  ImGui::FileBrowser &open_file_explorer();

	  private :
	  // ---- Non-owning observers into the MemoryManagement registry -------------
	  // Every pointer below is a non-owning observer.  Ownership lives in the
	  // global registry (PushGet in App::Alloc / Release in App::destroy); these
	  // are bound from MemoryManagement::GetInstance<T>() in Setup().  Never
	  // delete or reset them.  m_vk stays null until Setup() runs and so doubles
	  // as the "has Setup() run" sentinel used by the guards in the .cpp.
	  StyleEditor *m_style_editor;
  SDL_Window *m_window;
  vulkan_context *m_vk;
  bool *m_show_demo_window;
  bool *m_show_another_window;

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

  // ---- File explorer lifecycle --------------------------------------------
  // The file browser is NOT a persistent static. It is created on the heap
  // (MemoryManagement registry) when opened and destroyed when closed, so its
  // scanner + thumbnail worker threads start fresh each open and fully end on
  // close. m_explorer_thumb_dir is captured by SetThumbDir so a (re)created
  // browser can Setup() its thumbnail engine. See app_coordinator.cpp.
  std::filesystem::path m_explorer_thumb_dir;


  // Persist the browser's layout, end its threads, and release it from the
  // registry (destructor joins scanner + thumbnail workers). No-op if absent.
  void                close_file_explorer();
  void                apply_file_explorer_layout(ImGui::FileBrowser &fb, WindowStateToml const &state) const;
  void                export_file_explorer_layout(ImGui::FileBrowser &fb, WindowStateToml *state) const;

  // ---- Menu bar (rendering only) ------------------------------------------
  MainMenuBar m_menu;

  bool m_show_console = false;

  bool m_use_video_player_placebo = false;
};
