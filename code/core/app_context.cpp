/**
 * @file app_context.cpp
 * @brief Definitions for AppContext. All subsystem headers are included here so
 *        the unique_ptr members have complete types at construction/destruction.
 */

#include "pch.hpp"

#include "app_context.hpp"

#include "Image_viewer_panel.hpp"
#include "app_state_coordinator.hpp"
#include "bulk_image_open_queue.hpp"
#include "config_runtime.hpp"
#include "file_browser_context_menu.hpp"
#include "file_thumbnail_cache.hpp"
#include "history_preview.hpp"
#include "imgui_console.hpp"
#include "media_history_manager.hpp"
#include "media_load_handler.hpp"
#include "metadata_editor.hpp"
#include "open_image_dialogs.hpp"
#include "opened_files_window.hpp"
#include "video_context_menu.hpp"
#include "video_downloader.hpp"
#include "video_player.hpp"
#include "video_player_placebo.hpp"
#include "vulkan_emoji_atlas.hpp"

AppContext::AppContext()
	: m_viewer {std::make_unique<ImageViewerPanel>()}
	, m_open_image_dialogs {std::make_unique<OpenImageDialogs>()}
	, m_bulk_image_open {std::make_unique<BulkImageOpenQueue>()}
	, m_video_player {std::make_unique<VideoPlayer>()}
	, m_video_player_placebo {std::make_unique<VideoPlayerPlacebo>()}
	, m_video_downloader {std::make_unique<VideoDownloader>()}
	, m_config_runtime {std::make_unique<ConfigRuntime>()}
	, m_history_preview {std::make_unique<HistoryPreview>()}
	, m_opened_files_window {std::make_unique<OpenedFilesWindow>()}
	, m_video_context_menu {std::make_unique<VideoContextMenu>()}
	, m_fb_context_menu {std::make_unique<FileBrowserContextMenu>()}
	, m_thumb_cache {std::make_unique<FileThumbnailCache>()}
	, m_metadata_editor {std::make_unique<MetadataEditor>()}
	, m_history_mgr {std::make_unique<MediaHistoryManager>()}
	, m_load_handler {std::make_unique<MediaLoadHandler>()}
	, m_app_state {std::make_unique<AppStateCoordinator>()}
	, m_console {std::make_unique<ConsoleCommands>()} { }

// Out-of-line so the unique_ptr members see complete types here.
AppContext::~AppContext() = default;

ImageViewerPanel*       AppContext::Viewer() const noexcept { return m_viewer.get(); }
OpenImageDialogs*       AppContext::OpenImageDialogsPanel() const noexcept { return m_open_image_dialogs.get(); }
BulkImageOpenQueue*     AppContext::BulkImageOpen() const noexcept { return m_bulk_image_open.get(); }
VideoPlayer*            AppContext::Player() const noexcept { return m_video_player.get(); }
VideoPlayerPlacebo*     AppContext::PlayerPlacebo() const noexcept { return m_video_player_placebo.get(); }
VideoDownloader*        AppContext::Downloader() const noexcept { return m_video_downloader.get(); }
ConfigRuntime*          AppContext::Config() const noexcept { return m_config_runtime.get(); }
HistoryPreview*         AppContext::Preview() const noexcept { return m_history_preview.get(); }
OpenedFilesWindow*      AppContext::OpenedFiles() const noexcept { return m_opened_files_window.get(); }
VideoContextMenu*       AppContext::VideoMenu() const noexcept { return m_video_context_menu.get(); }
FileBrowserContextMenu* AppContext::FileBrowserMenu() const noexcept { return m_fb_context_menu.get(); }
FileThumbnailCache*     AppContext::ThumbCache() const noexcept { return m_thumb_cache.get(); }
MetadataEditor*         AppContext::Metadata() const noexcept { return m_metadata_editor.get(); }
MediaHistoryManager*    AppContext::History() const noexcept { return m_history_mgr.get(); }
MediaLoadHandler*       AppContext::LoadHandler() const noexcept { return m_load_handler.get(); }
AppStateCoordinator*    AppContext::AppState() const noexcept { return m_app_state.get(); }
ConsoleCommands*        AppContext::Console() const noexcept { return m_console.get(); }
VulkanEmojiAtlas*       AppContext::EmojiAtlas() const noexcept { return m_emoji_atlas.get(); }

AppContext* AppContext::GetInstance() noexcept {
	static std::unique_ptr<AppContext> instance = std::make_unique<AppContext>();
	return instance.get();
}


void AppContext::CreateEmojiAtlas(vulkan_context& vk) { m_emoji_atlas = std::make_unique<VulkanEmojiAtlas>(vk); }

void AppContext::DestroyEmojiAtlas() { m_emoji_atlas.reset(); }
