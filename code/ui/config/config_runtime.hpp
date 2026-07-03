#pragma once

#include "pch.hpp"

#include "window_state_toml.hpp"

namespace ImGui {
class FileBrowser;
}

/// Runtime configuration model.
/// Owns the settings that can be changed while the application is running
/// (preview sizes, hover/playback options, cache actions) and the callbacks
/// that apply them. The matching view lives in ConfigRuntimeUiContext, which
/// is granted friend access so it can bind ImGui widgets straight to this
/// state — keeping a single source of truth for ApplyLayout/ExportLayout.
class ConfigRuntime {

		friend class ConfigRuntimeUiContext;

	public:

		ConfigRuntime();

		/// Window-open flag (toggled from the main menu bar; persisted via Export).
		bool IsOpen;

		/// Restore values from persisted state (call after LoadWindowStateToml).
		void ApplyLayout(WindowStateToml const& state);

		/// Write current values into state (call before SaveWindowStateToml).
		void ExportLayout(WindowStateToml* state) const;

		/// Register a callback invoked when the user clicks "Clear Thumbnail Cache".
		void SetClearThumbnailCacheCallback(std::function<void()> cb);

		/// Register a callback invoked when the user clicks "Clear File Explorer Cache".
		void SetClearFileExplorerCacheCallback(std::function<void()> cb);

		/// Register a callback invoked when the user clicks "Clear Video Cache".
		void SetClearVideoCacheCallback(std::function<void()> cb);

		/// Register a callback invoked when the user clicks "Rebuild Video Cache".
		void SetRebuildVideoCacheCallback(std::function<void()> cb);

		/// Register a callback invoked when the user clicks "Clear History Metadata".
		void SetClearHistoryMetadataCallback(std::function<void()> cb);

		/// Register a callback invoked when the user clicks "Delete All Cache + Erase TOML".
		void SetDeleteAllCacheAndStateCallback(std::function<void()> cb);

		/// Register a callback invoked when the user clicks "Reopen App".
		void SetReopenAppCallback(std::function<void()> cb);

		/// Register a callback invoked when the video resume threshold is applied.
		void SetVideoResumeThresholdChangedCallback(std::function<void(int)> cb);

		[[nodiscard]] int VideoResumeThresholdSeconds() const;

		/// Current hold-to-accelerate playback speed multiplier.
		[[nodiscard]] float HoldSpeedMultiplier() const;

		/// Current arrow-key / button seek step in seconds.
		[[nodiscard]] int SeekStepSeconds() const;

		/// Register a callback invoked when hover preview enabled/delay changes.
		void SetHoverPreviewChangedCallback(std::function<void(bool, int)> cb);

		/// Register a callback invoked when global playback mode or loop changes (mode, loop).
		void SetVideoPlaybackChangedCallback(std::function<void(int mode, bool loop)> cb);

		/// Register a callback invoked when VSync changes.
		void SetVsyncChangedCallback(std::function<void(bool enabled)> cb);

		/// Returns the current pending VSync state.
		[[nodiscard]] bool VsyncEnabled() const;

		/// Current thumbnail storage backend ("bc1" | "png"). Read at file-browser
		/// setup; changes take effect on the next run.
		[[nodiscard]] std::string ThumbnailFormat() const;

		/// Per-type thumbnail quality presets ("original"|"high"|"medium"|"low"). Read at
		/// file-browser setup; changes take effect on the next run.
		[[nodiscard]] std::string ImageThumbnailTier() const;
		[[nodiscard]] std::string VideoThumbnailTier() const;

		/// Toggle VSync programmatically (fires the VSync changed callback).
		void SetVsyncEnabled(bool enabled);

		/// Register a callback invoked when "Restart All Threads" is clicked.
		void SetRestartAllThreadsCallback(std::function<void()> cb);

		/// Register a provider that returns the live FileBrowser (or nullptr when
		/// the file explorer is closed). Lets the Runtime Config window drive
		/// the per-mode thumbnail sizes directly. The provider is invoked each
		/// frame the section is visible — keep it cheap and null-safe.
		void SetFileBrowserProvider(std::function<ImGui::FileBrowser*()> provider);

		/// Draw a labelled single SCALE slider (a multiplier) + multiplier presets that
		/// resize `size` while preserving its current aspect ratio. `base_long_edge` is the
		/// 1.0x reference (the control's default long edge). Returns true if size changed.
		bool DrawPreviewSizeControl(char const* title, char const* drag_id, ImVec2& size,
			float base_long_edge);

	private:

		ImVec2                               m_pending_hover_size;
		ImVec2                               m_pending_seek_size;
		int                                  m_pending_video_resume_threshold_seconds;
		int                                  m_applied_video_resume_threshold_seconds;
		float                                m_pending_hold_speed_multiplier;
		int                                  m_pending_seek_step_seconds;
		std::function<void()>                m_on_clear_thumbnail_cache;
		std::function<void()>                m_on_clear_file_explorer_cache;
		std::function<void()>                m_on_clear_video_cache;
		std::function<void()>                m_on_rebuild_video_cache;
		std::function<void()>                m_on_clear_history_metadata;
		std::function<void()>                m_on_delete_all_cache_and_state;
		std::function<void()>                m_on_reopen_app;
		std::function<void(int)>             m_on_video_resume_threshold_changed;
		bool                                 m_pending_hover_preview_enabled;
		int                                  m_pending_hover_preview_delay_ms;
		bool                                 m_pending_hover_preview_sound;
		std::function<void(bool, int)>       m_on_hover_preview_changed;
		int                                  m_pending_global_playback_mode;
		bool                                 m_pending_global_loop_enabled;
		std::function<void(int, bool)>       m_on_video_playback_changed;
		bool                                 m_pending_vsync_enabled;
		std::function<void(bool)>            m_on_vsync_changed;
		std::string                          m_pending_thumbnail_format;
		std::string                          m_pending_image_thumbnail_tier;
		std::string                          m_pending_video_thumbnail_tier;
		std::function<void()>                m_on_restart_all_threads;
		std::function<ImGui::FileBrowser*()> m_fb_provider;
};
