#pragma once
#include "pch.hpp"

/**
 * @file app_context.hpp
 * @brief Central ownership/lifetime manager for all top-level subsystems.
 *
 * AppContext is the single owner of every long-lived subsystem object in the
 * application.  It holds each one in a std::unique_ptr (sole ownership,
 * deterministic destruction order) and hands out *non-owning* raw pointers
 * through typed getters.  Collaborators store those raw pointers as observers;
 * they never own and never extend the lifetime of a subsystem.
 *
 * Why unique_ptr (and not shared_ptr):
 *   - There is exactly one owner of each subsystem — this class.  Ownership is
 *     never shared, so reference counting would add atomic overhead and blur
 *     who is responsible for teardown.
 *   - Destruction order is defined here, in one place, by member declaration
 *     order (subsystems are torn down in reverse).  GPU/thread teardown is
 *     driven explicitly via Shutdown() on the owner before this object dies.
 *   - shared_ptr is only justified when an object's lifetime must outlive its
 *     creator in an indeterminate way — e.g. a flag captured by a *detached*
 *     worker thread (see ImGuiConsole::Alive_).  None of these subsystems fit
 *     that shape: their worker threads are std::jthread members that join on
 *     destruction, so the object always outlives its own threads.
 *
 * See docs/memory_management.md for the full evaluation.
 */

// ---- Forward declarations (kept opaque; full types live in the .cpp) -------
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
class MetadataEditor;
class MediaHistoryManager;
class MediaLoadHandler;
class AppStateCoordinator;
class ConsoleCommands;
class VulkanEmojiAtlas;


class AppContext {
    public:

	/// Constructs every subsystem that does not depend on the Vulkan context.
	/// The GPU-dependent emoji atlas is deferred to CreateEmojiAtlas().
	AppContext();
	~AppContext();

	AppContext(const AppContext&)		 = delete;
	AppContext& operator=(const AppContext&) = delete;

	static AppContext* GetInstance() noexcept;
	// ---- Non-owning getters: never null after construction --------------------
	// (Exception: EmojiAtlas() is null until CreateEmojiAtlas() runs.)
	[[nodiscard]] ImageViewerPanel*	      Viewer() const noexcept { return m_viewer.get(); }
	[[nodiscard]] OpenImageDialogs*	      OpenImageDialogsPanel() const noexcept { return m_open_image_dialogs.get(); }
	[[nodiscard]] BulkImageOpenQueue*     BulkImageOpen() const noexcept { return m_bulk_image_open.get(); }
	[[nodiscard]] VideoPlayer*	      Player() const noexcept { return m_video_player.get(); }
	[[nodiscard]] VideoPlayerPlacebo*     PlayerPlacebo() const noexcept { return m_video_player_placebo.get(); }
	[[nodiscard]] VideoDownloader*	      Downloader() const noexcept { return m_video_downloader.get(); }
	[[nodiscard]] ConfigRuntime*	      Config() const noexcept { return m_config_runtime.get(); }
	[[nodiscard]] HistoryPreview*	      Preview() const noexcept { return m_history_preview.get(); }
	[[nodiscard]] OpenedFilesWindow*      OpenedFiles() const noexcept { return m_opened_files_window.get(); }
	[[nodiscard]] VideoContextMenu*	      VideoMenu() const noexcept { return m_video_context_menu.get(); }
	[[nodiscard]] FileBrowserContextMenu* FileBrowserMenu() const noexcept { return m_fb_context_menu.get(); }
	[[nodiscard]] FileThumbnailCache*     ThumbCache() const noexcept { return m_thumb_cache.get(); }
	[[nodiscard]] MetadataEditor*	      Metadata() const noexcept { return m_metadata_editor.get(); }
	[[nodiscard]] MediaHistoryManager*    History() const noexcept { return m_history_mgr.get(); }
	[[nodiscard]] MediaLoadHandler*	      LoadHandler() const noexcept { return m_load_handler.get(); }
	[[nodiscard]] AppStateCoordinator*    AppState() const noexcept { return m_app_state.get(); }
	[[nodiscard]] ConsoleCommands*	      Console() const noexcept { return m_console.get(); }
	[[nodiscard]] VulkanEmojiAtlas*	      EmojiAtlas() const noexcept { return m_emoji_atlas.get(); }

	// ---- GPU-dependent, two-phase init ---------------------------------------
	/// Build the emoji atlas once the Vulkan context is available (from Setup()).
	void CreateEmojiAtlas(vulkan_context& vk);
	/// Release the emoji atlas GPU resources before ImGui_ImplVulkan_Shutdown.
	void DestroyEmojiAtlas();

    private:

	// Declaration order == construction order; destruction is the reverse.
	// Kept identical to the historical MainMenuBar member order so teardown
	// sequencing is unchanged by the centralisation.
	std::unique_ptr<ImageViewerPanel>	m_viewer;
	std::unique_ptr<OpenImageDialogs>	m_open_image_dialogs;
	std::unique_ptr<BulkImageOpenQueue>	m_bulk_image_open;
	std::unique_ptr<VideoPlayer>		m_video_player;
	std::unique_ptr<VideoPlayerPlacebo>	m_video_player_placebo;
	std::unique_ptr<VideoDownloader>	m_video_downloader;
	std::unique_ptr<ConfigRuntime>		m_config_runtime;
	std::unique_ptr<HistoryPreview>		m_history_preview;
	std::unique_ptr<OpenedFilesWindow>	m_opened_files_window;
	std::unique_ptr<VideoContextMenu>	m_video_context_menu;
	std::unique_ptr<FileBrowserContextMenu> m_fb_context_menu;
	std::unique_ptr<FileThumbnailCache>	m_thumb_cache;
	std::unique_ptr<MetadataEditor>		m_metadata_editor;
	std::unique_ptr<MediaHistoryManager>	m_history_mgr;
	std::unique_ptr<MediaLoadHandler>	m_load_handler;
	std::unique_ptr<AppStateCoordinator>	m_app_state;
	std::unique_ptr<ConsoleCommands>	m_console;
	std::unique_ptr<VulkanEmojiAtlas>	m_emoji_atlas; // built later, torn down first
};
