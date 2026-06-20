/**
 * @file app_coordinator.cpp
 * @brief Implementation of the top-level UI/subsystem coordinator.
 */

#include "pch.hpp"



#include "app_coordinator.hpp"

#include "Memory_management.hpp"
#include "app_runtime_state.hpp"
#include "sdl3_context.hpp"
#include "vulkan_context.hpp"

#include "Image_viewer_panel.hpp"
#include "app_state_coordinator.hpp"
#include "bulk_image_open_queue.hpp"
#include "config_runtime.hpp"
#include "file_browser_context_menu.hpp"
#include "history_preview.hpp"
#include "media_history_manager.hpp"
#include "media_load_handler.hpp"
#include "open_image_dialogs.hpp"
#include "opened_files_window.hpp"
#include "style_editor.hpp"
#include "video_context_menu.hpp"
#include "video_downloader.hpp"
#include "video_playback_mode.hpp"
#include "video_player.hpp"
#include "video_player_placebo.hpp"
#include "window_fullscreen_utils.hpp"
#include "window_state_toml.hpp"

#include "file_browser_ui.hpp"
#include "file_thumbnail_cache.hpp"
#include "imgui_console.hpp"
#include "metadata_editor.hpp"
#include "vulkan_emoji_atlas.hpp"

// ============================================================================
// File-local helpers
// ============================================================================

namespace {
// The file browser lives in the MemoryManagement registry, created on open and
// released on close. fb_ptr() returns the current heap instance, or nullptr when
// the explorer is closed — every call site must tolerate null.
ImGui::FileBrowser *fb_ptr() { return MemoryManagement::Get().GetSubobject<ImGui::FileBrowser>(); }

bool &GetShowFileExplorerFlag() {
	static bool show_file_explorer = false;
	return show_file_explorer;
}

bool IsHoverPreviewMediaPath(std::filesystem::path const &path) {
	if (VideoPlayer::is_video_path(path))
		return true;

	std::string const ext = [&]() {
		std::string s = path.extension().string();
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return s;
	}();

	static std::unordered_set<std::string> const k_image_exts
		= {".jpg", ".jpeg", ".png", ".bmp", ".tga", ".gif", ".webp"};
	return k_image_exts.count(ext) > 0;
}

bool UseVideoPlayerPlacebo() {
	char const *env = std::getenv("IMGUI_USE_VPP");
	if (!env)
		return false;

	std::string value(env);
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return value == "1" || value == "true" || value == "on" || value == "yes";
}
} // namespace

// ============================================================================
// Constructor / destructor
// ============================================================================

AppCoordinator::AppCoordinator()
	: request_quit {false}
	, request_reopen {false}
	, m_style_editor {nullptr}
	, m_window {nullptr}
	, m_vk {nullptr}
	, m_show_demo_window {nullptr}
	, m_show_another_window {nullptr}
	, m_use_video_player_placebo {UseVideoPlayerPlacebo()} {
	// All subsystem observers are bound from the MemoryManagement registry in
	// Setup(); the registry-owned objects are PushGet'd in App::Alloc() before
	// this coordinator. The constructor only captures the player-mode env flag.
}

// Destructor must be defined where all unique_ptr types are complete.
AppCoordinator::~AppCoordinator() = default;

// ============================================================================
// Public lifecycle
// ============================================================================

void AppCoordinator::Setup(std::function<void(bool)> on_vsync_changed) {
	// Phase-2 centralization: instead of App threading these in, resolve the
	// former external dependencies straight from the MemoryManagement registry.
	// All of them were PushGet'd in App::Alloc() before KickStart() calls Setup(),
	// so GetInstance<T>() (fatal-on-miss) always finds them here.
	auto *rt   = MemoryManagement::GetInstance<AppRuntimeState>();
	m_style_editor        = MemoryManagement::GetInstance<StyleEditor>();
	m_window              = MemoryManagement::GetInstance<sdl3_context>()->window;
	m_vk                  = MemoryManagement::GetInstance<vulkan_context>();
	m_show_demo_window    = &rt->showDemoWindow;
	m_show_another_window = &rt->showAnotherWindow;

	// Bind the subsystem observers from the registry. Each was PushGet'd in
	// App::Alloc() before this coordinator, so GetInstance<T>() (fatal-on-miss)
	// always resolves here. The EmojiAtlas is GPU-dependent and bound below.
	m_viewer               = MemoryManagement::GetInstance<ImageViewerPanel>();
	m_open_image_dialogs   = MemoryManagement::GetInstance<OpenImageDialogs>();
	m_bulk_image_open      = MemoryManagement::GetInstance<BulkImageOpenQueue>();
	m_video_player         = MemoryManagement::GetInstance<VideoPlayer>();
	m_video_player_placebo = MemoryManagement::GetInstance<VideoPlayerPlacebo>();
	m_video_downloader     = MemoryManagement::GetInstance<VideoDownloader>();
	m_config_runtime       = MemoryManagement::GetInstance<ConfigRuntime>();
	m_history_preview      = MemoryManagement::GetInstance<HistoryPreview>();
	m_opened_files_window  = MemoryManagement::GetInstance<OpenedFilesWindow>();
	m_video_context_menu   = MemoryManagement::GetInstance<VideoContextMenu>();
	m_fb_context_menu      = MemoryManagement::GetInstance<FileBrowserContextMenu>();
	m_thumb_cache          = MemoryManagement::GetInstance<FileThumbnailCache>();
	m_metadata_editor      = MemoryManagement::GetInstance<MetadataEditor>();
	m_history_mgr          = MemoryManagement::GetInstance<MediaHistoryManager>();
	m_load_handler         = MemoryManagement::GetInstance<MediaLoadHandler>();
	m_app_state            = MemoryManagement::GetInstance<AppStateCoordinator>();
	m_console              = MemoryManagement::GetInstance<ConsoleCommands>();

	curl_global_init(CURL_GLOBAL_DEFAULT);

	m_open_image_dialogs->setup(m_window);
	m_viewer->setup(m_window);
	m_video_player->bind_context(m_vk);
	m_video_player->setup(m_vk);
	m_video_player_placebo->bind_context(m_vk);
	m_video_player_placebo->setup(m_vk);

	if (m_use_video_player_placebo) {
		m_video_player_placebo->set_downloader(m_video_downloader);
		m_video_player_placebo->set_resume_persist_min_duration_seconds(m_config_runtime->VideoResumeThresholdSeconds());
	} else {
		m_video_player->set_downloader(m_video_downloader);
		m_video_player->set_resume_persist_min_duration_seconds(m_config_runtime->VideoResumeThresholdSeconds());
	}

	m_config_runtime->SetVideoResumeThresholdChangedCallback([this](int seconds) {
		if (m_use_video_player_placebo)
			m_video_player_placebo->set_resume_persist_min_duration_seconds(seconds);
		else
			m_video_player->set_resume_persist_min_duration_seconds(seconds);
	});
	m_config_runtime->SetVsyncChangedCallback(std::move(on_vsync_changed));
	m_video_context_menu->setup(m_window);
	m_video_context_menu->set_on_save_success([this](std::string const &source, std::filesystem::path const &saved_path) {
		m_history_mgr->replace_with_saved_file(source, saved_path, *m_opened_files_window);
		if (m_use_video_player_placebo)
			m_video_player_placebo->replace_source_with_saved_file(source, saved_path);
		else
			m_video_player->replace_source_with_saved_file(source, saved_path);
	});
	m_video_context_menu->set_playback_mode_callbacks(
		[this](std::string const &source) {
			return m_use_video_player_placebo
				? m_video_player_placebo->can_toggle_hwdec(source)
				: m_video_player->can_toggle_hwdec(source);
		},
		[this](std::string const &source) {
			bool const hwdec = m_use_video_player_placebo
				? m_video_player_placebo->is_hwdec_enabled(source)
				: m_video_player->is_hwdec_enabled(source);
			if (m_use_video_player_placebo)
				return static_cast<int>(VideoPlaybackMode::NvdecLibplacebo);
			return hwdec ? static_cast<int>(VideoPlaybackMode::NvdecMpv) : static_cast<int>(VideoPlaybackMode::SwMpv);
		},
		[this](std::string const &source, int mode) {
			int const  next_mode    = sanitize_video_playback_mode(mode);
			bool const next_hwdec   = mode_uses_hwdec(next_mode);
			bool const next_placebo = mode_uses_libplacebo(next_mode);

			int const resume_pos = m_use_video_player_placebo
				? m_video_player_placebo->persisted_position_seconds(source)
				: m_video_player->persisted_position_seconds(source);
			m_history_mgr->set_playback_mode(source, next_mode, *m_opened_files_window, resume_pos);

			m_use_video_player_placebo = next_placebo;
			m_load_handler->set_use_video_player_placebo(m_use_video_player_placebo);
			m_history_preview->set_use_video_player_placebo(m_use_video_player_placebo);

			m_video_player->set_all_hwdec(next_hwdec);
			m_video_player_placebo->set_all_hwdec(next_hwdec);
		});
	m_video_context_menu->set_vsync_callbacks([this]() { return m_config_runtime->VsyncEnabled(); },
		[this](bool enabled) { m_config_runtime->SetVsyncEnabled(enabled); });
	m_history_preview->setup(m_vk, m_video_player, m_viewer, m_video_player_placebo, m_use_video_player_placebo);


	m_fb_context_menu->setup(m_window);


	m_app_state->setup(m_history_mgr, m_config_runtime, m_history_preview, m_opened_files_window, m_video_downloader);

	auto const bind_player_menus = [this](auto *player) {
		player->set_context_menu(
			m_video_context_menu,
			[this](std::string const &src) -> WindowStateToml::ImageHistoryEntry * {
				for (auto &h : m_history_mgr->entries())
					if (h.source == src)
						return &h;
				return nullptr;
			},
			[this](std::string const &source) { m_history_mgr->erase(source, *m_opened_files_window); });

		player->set_player_menu_callbacks([this]() { m_open_image_dialogs->begin_open_image_dialog(); },
			[this]() { m_open_image_dialogs->open_url_popup(); },
			[this](std::string const &source, std::string const &kind) {
				if (kind == "file")
					m_open_image_dialogs->queue_path(source);
				else
					m_open_image_dialogs->queue_url(source);
			},
			[this]() -> std::vector<WindowStateToml::ImageHistoryEntry> const & { return m_history_mgr->entries(); },
			m_history_preview,
			[this](std::string const &video_source) { m_app_state->toggle_startup_video(video_source); },
			[this](std::string const &video_source) { return m_app_state->is_startup_video_fixed(video_source); },
			[this]() { return is_window_fullscreen(m_window); },
			[this](bool enabled) { set_window_fullscreen(m_window, enabled); });
	};

	// Wire menu/fullscreen callbacks on BOTH players.  The active player can be
	// switched at runtime (e.g. selecting "NVDEC libplacebo" flips
	// m_use_video_player_placebo), and the load/restore paths route videos to
	// whichever player is then active.  Binding only the initially-active player
	// would leave the other one with empty std::function callbacks, which crash
	// when invoked during draw.
	bind_player_menus(m_video_player_placebo);
	bind_player_menus(m_video_player);

	m_config_runtime->SetClearHistoryMetadataCallback([this]() { m_history_mgr->clear(*m_opened_files_window); });
	m_config_runtime->SetDeleteAllCacheAndStateCallback([this]() { m_app_state->clear_all_cache_and_state(); });
	m_config_runtime->SetReopenAppCallback([this]() {
		request_reopen = true;
		request_quit   = true;
	});
	m_config_runtime->SetVideoPlaybackChangedCallback([this](int mode, bool loop) {
		int const  next_mode    = sanitize_video_playback_mode(mode);
		bool const next_hwdec   = mode_uses_hwdec(next_mode);
		bool const next_placebo = mode_uses_libplacebo(next_mode);

		m_use_video_player_placebo = next_placebo;
		m_load_handler->set_use_video_player_placebo(m_use_video_player_placebo);
		m_history_preview->set_use_video_player_placebo(m_use_video_player_placebo);

		if (m_use_video_player_placebo) {
			m_video_player_placebo->set_all_hwdec(next_hwdec);
			m_video_player_placebo->set_all_loop(loop);
		} else {
			m_video_player->set_all_hwdec(next_hwdec);
			m_video_player->set_all_loop(loop);
		}
		m_video_player_placebo->reconfigure(VideoPlayerPlacebo::Config {
			.enable_hwdec = next_hwdec,
			.prefer_nvdec = next_hwdec,
		});
	});
	m_config_runtime->SetRestartAllThreadsCallback([this]() {
		if (m_use_video_player_placebo)
			m_video_player_placebo->restart_all_threads();
		else
			m_video_player->restart_all_threads();
	});

	m_opened_files_window->SetEraseHistoryEntryCallback([this](std::string const &source) {
		m_history_mgr->erase(source, *m_opened_files_window);
	});
	m_opened_files_window->SetRestartPreviewCallback([this]() {
		if (m_use_video_player_placebo)
			m_video_player_placebo->restart_hover_preview();
		else
			m_video_player->restart_hover_preview();
	});
	m_opened_files_window->SetRescanTomlCallback([this]() {
		if (!m_app_state->state_path().empty())
			LoadOpenedFilesHistoryFromToml(m_app_state->state_path());
	});
	m_opened_files_window->SetMenuShortcutsCallbacks([this]() { m_open_image_dialogs->begin_open_image_dialog(); },
		[this]() { m_open_image_dialogs->open_url_popup(); },
		[this]() {
			std::vector<std::string> const open_sources
				= m_use_video_player_placebo ? m_video_player_placebo->open_sources() : m_video_player->open_sources();
			if (open_sources.empty())
				return;
			bool const should_restore = !m_app_state->are_all_startup_videos_fixed(open_sources);
			m_app_state->set_startup_video_for_sources(open_sources, should_restore);
		},
		[this]() {
			std::vector<std::string> const open_sources
				= m_use_video_player_placebo ? m_video_player_placebo->open_sources() : m_video_player->open_sources();
			return m_app_state->are_all_startup_videos_fixed(open_sources);
		});

	m_opened_files_window->SetQuitCallback([this]() { request_quit = true; });

	m_load_handler->setup(m_viewer, m_video_player, m_video_player_placebo, m_bulk_image_open, m_history_mgr,
		m_video_downloader, m_opened_files_window, m_vk, m_use_video_player_placebo);

	// ---- Console -------------------------------------------------------
	// ConsoleCommands is registry-owned; m_console is the observer bound in Setup().
	m_console->OnQuit       = [this]() { request_quit = true; };
	m_console->OnDemoToggle = [this](bool on) {
		if (m_show_demo_window)
			*m_show_demo_window = on;
	};
	m_console->OnStyleChange = [](int which) {
		switch (which) {
		case 0:
			ImGui::StyleColorsDark();
			break;
		case 1:
			ImGui::StyleColorsLight();
			break;
		case 2:
			ImGui::StyleColorsClassic();
			break;
		default:
			break;
		}
	};
	// VulkanEmojiAtlas is GPU-dependent (needs vulkan_context), so it is created
	// here rather than in App::Alloc(); Shutdown() Releases it before ImGui's
	// Vulkan backend dies. PushGet returns the owned observer.
	m_emoji_atlas = MemoryManagement::Get().PushGet<VulkanEmojiAtlas>("EmojiAtlas", *m_vk);
	// Build the atlas lazily on first Draw() — deferred to avoid blocking Setup.
}

void AppCoordinator::Shutdown() {
	if (!m_vk)
		return;

	m_bulk_image_open->shutdown();
	m_video_player_placebo->shutdown();
	m_video_player->shutdown();
	m_video_downloader->shutdown();
	m_history_preview->shutdown();
	m_viewer->shutdown(*m_vk);
	close_file_explorer(); // export layout + end scanner/thumbnail threads + free textures (vk/ImGui alive)
	MemoryManagement::Get().Release<VulkanEmojiAtlas>(); // frees GPU resources before ImGui Vulkan shutdown
	m_emoji_atlas = nullptr;

	curl_global_cleanup();
}



// ============================================================================
// History + config forwarding
// ============================================================================

void AppCoordinator::ApplyHistory(WindowStateToml const &state) { m_app_state->apply_history(state); }

void AppCoordinator::ApplyRuntimeConfig(WindowStateToml const &state) {
	m_app_state->apply_runtime_config(state);

	// The browser does not exist yet at startup — its layout is applied when
	// open_file_explorer() creates it (it reads the same persisted state). Here we
	// only restore the open/closed flag; the render loop creates it next frame.
	if (state.show_file_explorer_window)
		GetShowFileExplorerFlag() = true;
	m_show_console = state.show_console_window;
}

bool AppCoordinator::LoadOpenedFilesHistoryFromToml(std::filesystem::path const &file_path) {
	return m_app_state->load_opened_files_history_from_toml(file_path);
}

void AppCoordinator::SetStatePath(std::filesystem::path const &file_path) { m_app_state->set_state_path(file_path); }

void AppCoordinator::ExportHistory(WindowStateToml *state) {
	if (m_use_video_player_placebo)
		m_video_player_placebo->sync_history_state(m_history_mgr->entries());
	else
		m_video_player->sync_history_state(m_history_mgr->entries());
	m_app_state->export_history(state);
}

void AppCoordinator::ExportRuntimeConfig(WindowStateToml *state) const {
	m_app_state->export_runtime_config(state);

	// If the explorer is open, capture its live layout. If it is closed, the layout
	// was already written into `state` when it was closed (close_file_explorer), so
	// we leave those values untouched.
	if (auto *fb = fb_ptr())
		export_file_explorer_layout(*fb, state);
	state->show_file_explorer_window = GetShowFileExplorerFlag();
	state->show_console_window       = m_show_console;
}

void AppCoordinator::apply_file_explorer_layout(ImGui::FileBrowser &fb, WindowStateToml const &state) const {
	if (!state.file_explorer_recent_directories.empty()) {
		std::vector<std::filesystem::path> dirs;
		dirs.reserve(state.file_explorer_recent_directories.size());
		for (auto const &dir : state.file_explorer_recent_directories)
			if (!dir.empty())
				dirs.emplace_back(dir);
		fb.SetRecentDirectories(dirs);
	}
	fb.SetSortModeIndex(state.file_explorer_sort_mode);
	fb.SetSortAscending(state.file_explorer_sort_ascending);
	fb.SetViewMode(static_cast<ImGui::FileBrowser::ViewMode>(state.file_explorer_view_mode));
	fb.SetMediaFilter(static_cast<ImGui::FileBrowser::MediaFilter>(state.file_explorer_media_filter));
	fb.SetPreviewEnabled(state.file_explorer_preview);
	fb.SetShowThumbnails(state.file_explorer_show_thumbnails);
	fb.SetKeepOpen(state.file_explorer_keep_open);
	if (state.file_explorer_thumb_size)
		fb.SetThumbnailSize(ImVec2 {state.file_explorer_thumb_size->x, state.file_explorer_thumb_size->y});
	if (state.file_explorer_grid_thumb_size)
		fb.SetGridThumbnailSize(ImVec2 {state.file_explorer_grid_thumb_size->x, state.file_explorer_grid_thumb_size->y});
	if (!state.file_explorer_last_directory.empty())
		fb.SetDirectory(state.file_explorer_last_directory);
}

void AppCoordinator::export_file_explorer_layout(ImGui::FileBrowser &fb, WindowStateToml *state) const {
	state->file_explorer_last_directory  = fb.GetDirectory().string();
	state->file_explorer_sort_mode       = fb.GetSortModeIndex();
	state->file_explorer_sort_ascending  = fb.GetSortAscending();
	state->file_explorer_view_mode       = static_cast<int>(fb.GetViewMode());
	state->file_explorer_media_filter    = static_cast<int>(fb.GetMediaFilter());
	state->file_explorer_preview         = fb.IsPreviewEnabled();
	state->file_explorer_show_thumbnails = fb.GetShowThumbnails();
	state->file_explorer_keep_open       = fb.GetKeepOpen();
	{
		ImVec2 const ts                 = fb.GetThumbnailSize();
		state->file_explorer_thumb_size = WindowStateToml::Vec2Toml {ts.x, ts.y};
	}
	{
		ImVec2 const gs                      = fb.GetGridThumbnailSize();
		state->file_explorer_grid_thumb_size = WindowStateToml::Vec2Toml {gs.x, gs.y};
	}
	state->file_explorer_recent_directories.clear();
	for (auto const &dir : fb.GetRecentDirectories())
		state->file_explorer_recent_directories.push_back(dir.string());
}

ImGui::FileBrowser &AppCoordinator::open_file_explorer() {
	if (auto *existing = fb_ptr())
		return *existing; // already open — idempotent

	// Heap-allocate a brand-new browser via the registry. Constructor flags match the
	// old static. Everything below re-establishes the per-instance configuration that
	// a fresh object needs, so each open is a clean start with fresh worker threads.
	auto *fb = MemoryManagement::Get().PushGet<ImGui::FileBrowser>("FileExplorer",
		ImGuiFileBrowserFlags_Window | ImGuiFileBrowserFlags_EditPathString | ImGuiFileBrowserFlags_CreateNewDir
			| ImGuiFileBrowserFlags_MultipleSelection);
	fb->SetTitle("File Explorer");
	fb->SetWindowSize(900, 560);
	fb->SetTypeFilters({".*"});

	fb->SetHoverFileCallback([this](std::filesystem::path const &path) {
		if (!IsHoverPreviewMediaPath(path))
			return;
		WindowStateToml::ImageHistoryEntry tmp;
		tmp.source = path.string();
		tmp.title  = path.filename().string();
		tmp.kind   = "file";
		m_history_preview->draw_for_hover(tmp);
	});
	fb->SetContextMenuCallback([this](std::filesystem::path const &path) {
		auto res = m_fb_context_menu->draw(path);
		if (res.open)
			m_open_image_dialogs->queue_path(res.open_path.string());
	});
	fb->SetRebuildThumbnailCallback([](std::filesystem::path const &path) {
		if (auto *b = fb_ptr())
			b->RebuildThumbnail(path);
	});

	// Start the async thumbnail engine (fresh scanner + video worker threads), then
	// restore the persisted layout (directory, sort, view, thumbnail sizes).
	if (m_vk && !m_explorer_thumb_dir.empty())
		fb->Setup(m_vk, m_explorer_thumb_dir);
	apply_file_explorer_layout(*fb, *MemoryManagement::GetInstance<WindowStateToml>());
	fb->Open();
	return *fb;
}

void AppCoordinator::close_file_explorer() {
	auto *fb = fb_ptr();
	if (!fb)
		return;
	// Persist layout BEFORE destroying so the next open restores it (this also runs
	// before ExportRuntimeConfig at shutdown, which then sees no browser).
	export_file_explorer_layout(*fb, MemoryManagement::GetInstance<WindowStateToml>());
	fb->ShutdownThumbnails(); // stop engine + free GPU textures while Vulkan/ImGui are alive
	// Release runs ~FileBrowser now: m_thumbnails + m_scanner destruct, joining the
	// video worker(s) and the scanner jthread. A later open creates a genuinely fresh one.
	MemoryManagement::Get().Release<ImGui::FileBrowser>();
}
	
void AppCoordinator::SetMediaPolicy(bool allow_video, bool allow_image) {
	m_load_handler->set_media_policy(allow_video, allow_image);
}

void AppCoordinator::ConsoleLog(std::string line) {
	if (m_console)
		m_console->AddLogThreadSafe(std::move(line));
}

void AppCoordinator::ShowConsole() { m_show_console = true; }

void AppCoordinator::SetThumbDir(std::filesystem::path const &dir) {
	m_app_state->set_thumb_dir(dir);

	// Remember the thumbnail directory so open_file_explorer() can Setup() the
	// browser's async thumbnail engine each time a fresh browser is created.
	m_explorer_thumb_dir = dir;

	if (m_vk) {
		// Clear-cache may be triggered from the Runtime Config window while the
		// explorer is closed (no browser): in that case the in-memory cache is
		// already gone with the destroyed browser, so a missing browser is a no-op.
		m_config_runtime->SetClearFileExplorerCacheCallback([]() {
			if (auto *fb = fb_ptr())
				fb->ClearThumbnailCache();
		});

		// Context-menu extras run from the browser's own context menu, so the
		// browser is alive here; null-guarded anyway for safety.
		m_fb_context_menu->SetExtraItemsCallback([this](std::filesystem::path const &path) {
			auto *fb = fb_ptr();
			if (!fb)
				return;
			if (ImGui::MenuItem("Rebuild Thumbnail"))
				fb->RebuildThumbnail(path);
			if (ImGui::MenuItem("Edit Tags\xe2\x80\xa6")) {
				auto selected = fb->GetMultiSelected();
				// Remove directories from the selection; only tag files.
				selected.erase(
					std::remove_if(selected.begin(), selected.end(),
						[](std::filesystem::path const &p) { return std::filesystem::is_directory(p); }),
					selected.end());
				if (selected.size() > 1)
					m_metadata_editor->OpenMultiple(selected);
				else
					m_metadata_editor->Open(path);
			}
		});
	}
}

void AppCoordinator::SetDownloadCacheDir(std::filesystem::path const &dir) { m_app_state->set_download_cache_dir(dir); }

bool AppCoordinator::HandleSdlEvent(SDL_Event const &event) {
	if (event.type != SDL_EVENT_KEY_DOWN)
		return false;

	SDL_Keycode const key = event.key.key;

	// The file-explorer hover-preview popup, while actively playing, owns the arrow
	// keys: Left/Right seek and Up/Down change its volume. These helpers route to
	// whichever player owns the preview (the placebo player forwards to its inner
	// software hover player). When the popup owns a key we swallow the event so
	// ImGui's Selectable navigation can't move the file-browser selection — that
	// was the "arrow keys select another file instead of seeking" bug.
	auto const hover_previewing = [this]() -> bool {
		if (m_use_video_player_placebo && m_video_player_placebo)
			return m_video_player_placebo->is_hover_previewing();
		return m_video_player && m_video_player->is_hover_previewing();
	};
	auto const seek_hover = [this](double d) {
		if (m_use_video_player_placebo && m_video_player_placebo)
			m_video_player_placebo->seek_hover_preview(d);
		else if (m_video_player)
			m_video_player->seek_hover_preview(d);
	};
	auto const adjust_hover_vol = [this](int d) {
		if (m_use_video_player_placebo && m_video_player_placebo)
			m_video_player_placebo->adjust_hover_volume(d);
		else if (m_video_player)
			m_video_player->adjust_hover_volume(d);
	};

	// Space (tap = play/pause, hold = fast-forward) is handled per-frame in
	// VideoPlayer::update_space_hold_speed() / update_hover_space_hold_speed() so the
	// hold threshold is frame-accurate rather than tied to the OS key-repeat delay.
	// We deliberately do NOT consume Space here so ImGui keeps its live key state
	// for those FSMs; Space does not move the file-browser selection anyway.

	// Left/Right arrows seek by the configurable step. Same WantTextInput guard so
	// they still move the caret inside text fields.
	if ((key == SDLK_LEFT || key == SDLK_RIGHT) && !ImGui::GetIO().WantTextInput) {
		double const step  = m_config_runtime ? static_cast<double>(m_config_runtime->SeekStepSeconds()) : 5.0;
		double const delta = (key == SDLK_RIGHT) ? step : -step;

		if (hover_previewing()) {
			seek_hover(delta);
			return true; // swallow: keep the file-browser selection put
		}
		if (!m_use_video_player_placebo && m_video_player)
			m_video_player->seek_active_video(delta);
		return false;
	}

	// Up/Down adjust volume — hover preview first (and swallow so vertical list
	// navigation doesn't move the selection), else the active open video.
	if ((key == SDLK_UP || key == SDLK_DOWN) && !ImGui::GetIO().WantTextInput) {
		int const delta = (key == SDLK_UP) ? 5 : -5;
		if (hover_previewing()) {
			adjust_hover_vol(delta);
			return true; // swallow: keep the file-browser selection put
		}
		if (!m_use_video_player_placebo && m_video_player)
			m_video_player->adjust_active_volume(delta);
		return false;
	}
	if (key == SDLK_C && !ImGui::GetIO().WantTextInput) {
		if (!m_use_video_player_placebo && m_video_player)
			m_video_player->handle_media_key(SDLK_MUTE);
		return false;
	}
	if (key == SDLK_L && !ImGui::GetIO().WantTextInput) {
		if (!m_use_video_player_placebo && m_video_player)
			m_video_player->toggle_active_loop();
		return false;
	}

	switch (key) {
	case SDLK_MEDIA_PLAY:
	case SDLK_MEDIA_PAUSE:
	case SDLK_MEDIA_PLAY_PAUSE:
	case SDLK_MEDIA_STOP:
	case SDLK_MEDIA_NEXT_TRACK:
	case SDLK_MEDIA_PREVIOUS_TRACK:
	case SDLK_MEDIA_FAST_FORWARD:
	case SDLK_MEDIA_REWIND:
	case SDLK_MUTE:
		if (m_use_video_player_placebo) {
			// VideoPlayerPlacebo does not yet expose handle_media_key; skip.
		} else if (m_video_player) {
			m_video_player->handle_media_key(key);
		}
		break;
	default:
		break;
	}
	return false;
}

// ============================================================================
// Per-frame build
// ============================================================================

void AppCoordinator::Build() {
	// Step 1 — evict closed image windows
	if (m_vk)
		m_viewer->evict_closed(*m_vk);

	// Step 2 — finalise deferred video save
	m_video_context_menu->process_pending_save();

	// Step 2b — file browser context menu deferred ops (delete confirmation
	// modal)
	m_fb_context_menu->process_pending();

	// Step 3 — one-shot startup video restoration
	if (m_app_state->take_restore_videos_on_startup_pending())
		m_load_handler->restore_from_history();

	// Step 4 — drain completed background downloads
	for (auto const &result : m_video_downloader->take_completed()) {
		if (!result.ok)
			continue;

		for (auto &h : m_history_mgr->entries()) {
			if (h.source == result.url) {
				h.cached_path = result.cached_path.string();
				break;
			}
		}
		m_history_mgr->persist();
		if (m_use_video_player_placebo)
			m_video_player_placebo->notify_download_complete(result.url, result.cached_path);
		else
			m_video_player->notify_download_complete(result.url, result.cached_path);
	}

	// Steps 5–7 — delegate media loading
	m_load_handler->process_pending_paths(m_open_image_dialogs->take_pending_paths());
	m_load_handler->process_pending_urls(m_open_image_dialogs->take_pending_urls());
	m_load_handler->drain_bulk_queue();

	// Step 8 — menu bar rendering (delegated to MainMenuBar)
	MainMenuBar::MenuContext mc;
	mc.style_editor             = m_style_editor;
	mc.show_demo_window         = m_show_demo_window;
	mc.show_another_window      = m_show_another_window;
	mc.show_console             = &m_show_console;
	mc.show_file_explorer       = &GetShowFileExplorerFlag();
	mc.file_explorer            = fb_ptr(); // may be null while the explorer is closed
	mc.request_quit             = &request_quit;
	mc.use_video_player_placebo = m_use_video_player_placebo;
	if (!m_menu.Draw(mc))
		return;

	// Lifecycle edge detection: create the browser the frame the flag turns on,
	// destroy it (ending its threads) the frame it turns off or its window is closed.
	bool               &show_file_explorer = GetShowFileExplorerFlag();
	ImGui::FileBrowser *file_explorer      = fb_ptr();
	if (show_file_explorer && !file_explorer)
		file_explorer = &open_file_explorer();
	else if (!show_file_explorer && file_explorer) {
		close_file_explorer();
		file_explorer = nullptr;
	}
	if (file_explorer) {
		file_explorer->Display();
		if (file_explorer->HasSelected()) {
			auto const selected = file_explorer->GetSelected();
			m_open_image_dialogs->queue_path(selected.string());
			file_explorer->ClearSelected();
		}
		if (!file_explorer->IsOpened())
			show_file_explorer = false; // window closed (X) -> destroyed next frame
	}

	// Console window
	if (m_show_console && m_console)
		m_console->Draw("Console##main", &m_show_console);

	// Metadata editor window
	m_metadata_editor->Draw();

	// Step 9 — URL popup
	m_open_image_dialogs->draw_url_popup();

	// Steps 10–12 — subsystem rendering
	if (m_use_video_player_placebo)
		m_video_player_placebo->update_frames();
	else
		m_video_player->update_frames();
	m_config_runtime_ui.DrawUi(m_config_runtime);
	m_viewer->draw_windows();
	if (m_use_video_player_placebo)
		m_video_player_placebo->draw();
	else
		m_video_player->draw();

	int        focus_id  = -1;
	auto const activated = m_opened_files_window->draw(*m_viewer, *m_history_preview, &focus_id, m_video_context_menu);

	if (focus_id >= 0)
		m_viewer->request_focus(focus_id);

	if (activated.has_value()) {
		if (activated->kind == "file")
			m_open_image_dialogs->queue_path(activated->source);
		else if (activated->kind == "url")
			m_open_image_dialogs->queue_url(activated->source);
	}
}