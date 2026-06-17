#include "pch.hpp"

#include "app_state_coordinator.hpp"

#include "config_runtime.hpp"
#include "history_preview.hpp"
#include "media_history_manager.hpp"
#include "opened_files_window.hpp"
#include "video_downloader.hpp"
#include "video_player.hpp"
#include "window_state_toml.hpp"

namespace {

bool source_matches_history(std::string const &video_source, WindowStateToml::ImageHistoryEntry const &entry) {
	if (entry.source == video_source)
		return true;

	std::error_code             source_ec;
	std::filesystem::path const source_norm
		= std::filesystem::weakly_canonical(std::filesystem::path(video_source), source_ec);
	if (!source_ec && entry.source == source_norm.string())
		return true;

	if (VideoPlayer::is_video_url(entry.source) && !entry.cached_path.empty()) {
		if (entry.cached_path == video_source)
			return true;

		std::error_code             cached_ec;
		std::filesystem::path const cached_norm
			= std::filesystem::weakly_canonical(std::filesystem::path(entry.cached_path), cached_ec);
		if (!cached_ec && cached_norm.string() == video_source)
			return true;
		if (!cached_ec && !source_ec && cached_norm == source_norm)
			return true;
	}

	return false;
}

} // namespace

AppStateCoordinator::AppStateCoordinator()
	: m_history_mgr {nullptr}
	, m_config_runtime {nullptr}
	, m_history_preview {nullptr}
	, m_files_window {nullptr}
	, m_video_downloader {nullptr}
	, m_thumb_dir {}
	, m_download_cache_dir {}
	, m_state_path {}
	, m_restore_videos_on_startup_pending {true} { }

void AppStateCoordinator::setup(MediaHistoryManager *history_mgr, ConfigRuntime *config_runtime,
	HistoryPreview *history_preview, OpenedFilesWindow *files_window, VideoDownloader *video_downloader) {
	m_history_mgr      = history_mgr;
	m_config_runtime   = config_runtime;
	m_history_preview  = history_preview;
	m_files_window     = files_window;
	m_video_downloader = video_downloader;
}

void AppStateCoordinator::apply_history(WindowStateToml const &state) {
	m_history_mgr->apply(state, *m_files_window);
	m_restore_videos_on_startup_pending = true;
}

void AppStateCoordinator::apply_runtime_config(WindowStateToml const &state) {
	m_config_runtime->ApplyLayout(state);
	m_files_window->ApplyLayout(state);
}

bool AppStateCoordinator::load_opened_files_history_from_toml(std::filesystem::path const &file_path) {
	bool const loaded = m_files_window->load_history_from_toml(file_path);

	if (loaded) {
		WindowStateToml state;
		if (LoadWindowStateToml(file_path, state))
			m_history_mgr->apply(state, *m_files_window);

		m_restore_videos_on_startup_pending = true;
	}

	return loaded;
}

void AppStateCoordinator::set_state_path(std::filesystem::path const &file_path) {
	m_state_path = file_path;
	m_history_mgr->set_state_path(file_path);
}

void AppStateCoordinator::export_history(WindowStateToml *state) const { m_history_mgr->export_to(state); }

void AppStateCoordinator::export_runtime_config(WindowStateToml *state) const {
	m_config_runtime->ExportLayout(state);
	m_files_window->ExportLayout(state);
}

void AppStateCoordinator::set_thumb_dir(std::filesystem::path const &dir) {
	m_thumb_dir = dir;
	m_history_preview->set_thumb_dir(dir);

	m_config_runtime->SetClearThumbnailCacheCallback([this]() {
		std::error_code ec;

		for (auto const &entry : std::filesystem::directory_iterator(m_thumb_dir, ec)) {
			if (entry.path().extension() == ".png")
				std::filesystem::remove(entry.path(), ec);
		}

		for (auto &h : m_history_mgr->entries())
			h.thumbnail_path.clear();

		m_history_mgr->persist();
	});
}

void AppStateCoordinator::set_download_cache_dir(std::filesystem::path const &dir) {
	m_video_downloader->set_cache_dir(dir);
	m_download_cache_dir = dir;

	m_config_runtime->SetClearVideoCacheCallback([this]() {
		m_video_downloader->clear_cache();

		for (auto &h : m_history_mgr->entries())
			h.cached_path.clear();

		m_history_mgr->persist();
	});

	m_config_runtime->SetRebuildVideoCacheCallback([this]() {
		m_video_downloader->clear_cache();

		for (auto const &h : m_history_mgr->entries()) {
			if (VideoPlayer::is_video_url(h.source)) {
				auto const cached = m_video_downloader->get_or_enqueue(h.source);
				if (cached) {
					for (auto &entry : m_history_mgr->entries()) {
						if (entry.source == h.source) {
							entry.cached_path = cached->string();
							break;
						}
					}
				}
				continue;
			}

			if (VideoPlayer::is_video_path(h.source))
				continue;
		}

		m_history_mgr->persist();
	});
}

void AppStateCoordinator::clear_all_cache_and_state() {
	if (m_video_downloader)
		m_video_downloader->clear_cache();

	if (!m_thumb_dir.empty()) {
		std::error_code ec;
		for (auto const &entry : std::filesystem::directory_iterator(m_thumb_dir, ec)) {
			if (entry.path().extension() == ".png")
				std::filesystem::remove(entry.path(), ec);
		}
	}

	if (m_history_mgr && m_files_window)
		m_history_mgr->clear(*m_files_window);

	if (!m_state_path.empty()) {
		std::error_code ec;
		std::filesystem::remove(m_state_path, ec);
	}
}

bool AppStateCoordinator::is_startup_video_fixed(std::string const &video_source) const {
	for (auto const &entry : m_history_mgr->entries()) {
		bool const is_video = VideoPlayer::is_video_path(entry.source) || VideoPlayer::is_video_url(entry.source);
		if (!is_video)
			continue;
		if (!source_matches_history(video_source, entry))
			continue;
		if (entry.startup_restore)
			return true;
	}

	return false;
}

void AppStateCoordinator::set_startup_video(std::string const &video_source, bool should_restore) {
	bool history_changed = false;

	for (auto &entry : m_history_mgr->entries()) {
		bool const is_video = VideoPlayer::is_video_path(entry.source) || VideoPlayer::is_video_url(entry.source);
		if (!is_video)
			continue;
		if (!source_matches_history(video_source, entry))
			continue;

		if (entry.startup_restore != should_restore) {
			entry.startup_restore = should_restore;
			history_changed       = true;
		}
	}

	if (history_changed)
		m_history_mgr->persist();
}

void AppStateCoordinator::toggle_startup_video(std::string const &video_source) {
	set_startup_video(video_source, !is_startup_video_fixed(video_source));
}

bool AppStateCoordinator::are_all_startup_videos_fixed(std::vector<std::string> const &video_sources) const {
	if (video_sources.empty())
		return false;

	return std::all_of(video_sources.begin(), video_sources.end(), [this](std::string const &source) {
		return is_startup_video_fixed(source);
	});
}

void AppStateCoordinator::set_startup_video_for_sources(std::vector<std::string> const &video_sources,
	bool                                                                                should_restore) {
	for (auto const &source : video_sources)
		set_startup_video(source, should_restore);
}

std::filesystem::path const &AppStateCoordinator::state_path() const { return m_state_path; }

bool AppStateCoordinator::take_restore_videos_on_startup_pending() {
	bool const was_pending              = m_restore_videos_on_startup_pending;
	m_restore_videos_on_startup_pending = false;
	return was_pending;
}