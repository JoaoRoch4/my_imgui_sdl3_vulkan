#pragma once

#include "pch.hpp"

#include "window_state_toml.hpp"

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
	void ApplyLayout(const WindowStateToml& state);

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

	/// Register a callback invoked when hover preview enabled/delay changes.
	void SetHoverPreviewChangedCallback(std::function<void(bool, int)> cb);

	/// Register a callback invoked when global playback mode or loop changes (mode, loop).
	void SetVideoPlaybackChangedCallback(std::function<void(int mode, bool loop)> cb);

	/// Register a callback invoked when VSync changes.
	void SetVsyncChangedCallback(std::function<void(bool enabled)> cb);

	/// Returns the current pending VSync state.
	[[nodiscard]] bool VsyncEnabled() const;

	/// Toggle VSync programmatically (fires the VSync changed callback).
	void SetVsyncEnabled(bool enabled);

	/// Register a callback invoked when "Restart All Threads" is clicked.
	void SetRestartAllThreadsCallback(std::function<void()> cb);

	/// Draw a labelled DragFloat2 + preset buttons. Returns true if size changed.
	bool DrawPreviewSizeControl(const char* title, const char* drag_id, ImVec2& size);

    private:

	ImVec2			       m_pending_hover_size;
	ImVec2			       m_pending_seek_size;
	int			       m_pending_video_resume_threshold_seconds;
	int			       m_applied_video_resume_threshold_seconds;
	std::function<void()>	       m_on_clear_thumbnail_cache;
	std::function<void()>	       m_on_clear_file_explorer_cache;
	std::function<void()>	       m_on_clear_video_cache;
	std::function<void()>	       m_on_rebuild_video_cache;
	std::function<void()>	       m_on_clear_history_metadata;
	std::function<void()>	       m_on_delete_all_cache_and_state;
	std::function<void()>	       m_on_reopen_app;
	std::function<void(int)>       m_on_video_resume_threshold_changed;
	bool			       m_pending_hover_preview_enabled;
	int			       m_pending_hover_preview_delay_ms;
	bool			       m_pending_hover_preview_sound;
	std::function<void(bool, int)> m_on_hover_preview_changed;
	int			       m_pending_global_playback_mode;
	bool			       m_pending_global_loop_enabled;
	std::function<void(int, bool)> m_on_video_playback_changed;
	bool			       m_pending_vsync_enabled;
	std::function<void(bool)>      m_on_vsync_changed;
	std::function<void()>	       m_on_restart_all_threads;
};
