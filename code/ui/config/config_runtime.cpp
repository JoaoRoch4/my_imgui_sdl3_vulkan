#include "pch.hpp"

#include "config_runtime.hpp"

#include "Image_viewer_panel.hpp"
#include "video_hover_preview.hpp"
#include "video_playback_mode.hpp"
#include "video_seek_preview.hpp"
#include "video_ui_window.hpp"

namespace {

// Scale-multiplier presets for the preview-size control (1.0x == the control's default size).
constexpr float k_scale_presets[] = {0.5f, 1.0f, 2.0f, 3.0f};

} // namespace

ConfigRuntime::ConfigRuntime()
	: IsOpen {false}
	, m_pending_hover_size {VideoHoverPreview::preview_size}
	, m_pending_seek_size {VideoSeekPreview::preview_size}
	, m_pending_video_resume_threshold_seconds {WindowStateToml {}
			  .video_resume_persist_min_duration_seconds}
	, m_applied_video_resume_threshold_seconds {WindowStateToml {}
			  .video_resume_persist_min_duration_seconds}
	, m_pending_hold_speed_multiplier {WindowStateToml {}.video_hold_speed_multiplier}
	, m_pending_seek_step_seconds {WindowStateToml {}.video_seek_step_seconds}
	, m_on_clear_thumbnail_cache {nullptr}
	, m_on_clear_file_explorer_cache {nullptr}
	, m_on_clear_video_cache {nullptr}
	, m_on_rebuild_video_cache {nullptr}
	, m_on_clear_history_metadata {nullptr}
	, m_on_delete_all_cache_and_state {nullptr}
	, m_on_reopen_app {nullptr}
	, m_on_video_resume_threshold_changed {nullptr}
	, m_pending_hover_preview_enabled {VideoHoverPreview::enabled}
	, m_pending_hover_preview_delay_ms {static_cast<int>(VideoHoverPreview::hover_delay.count())}
	, m_pending_hover_preview_sound {VideoHoverPreview::preview_sound}
	, m_on_hover_preview_changed {nullptr}
	, m_pending_global_playback_mode {static_cast<int>(VideoPlaybackMode::SwMpv)}
	, m_pending_global_loop_enabled {false}
	, m_on_video_playback_changed {nullptr}
	, m_pending_vsync_enabled {WindowStateToml {}.vsync}
	, m_on_vsync_changed {nullptr}
	, m_pending_thumbnail_format {WindowStateToml {}.thumbnail_format}
	, m_pending_image_thumbnail_tier {WindowStateToml {}.image_thumbnail_tier}
	, m_pending_video_thumbnail_tier {WindowStateToml {}.video_thumbnail_tier}
	, m_on_restart_all_threads {nullptr} { }

// Single SCALE multiplier slider (replaces the old width/height DragFloat2). The slider
// scales the longer edge while the shorter edge follows the CURRENT aspect ratio, so width
// and height always move together — no distortion. 1.0x maps to `base_long_edge` (the
// control's default size), and the result is clamped to a sane pixel range.
// Returns true if the value changed.
bool ConfigRuntime::DrawPreviewSizeControl(char const* title, char const* drag_id, ImVec2& size,
	float base_long_edge) {

	constexpr float kMinPx = 80.0f, kMaxPx = 4000.0f;
	constexpr float kMinScale = 0.25f, kMaxScale = 3.0f;

	float const ref    = (base_long_edge > 0.0f) ? base_long_edge : 1.0f; // 1.0x reference
	float const aspect = (size.y > 0.0f) ? (size.x / size.y) : 1.0f; // w/h, preserved

	// Re-scale `size` to `s`x the reference, keeping the current aspect.
	auto const apply_scale = [&](float s) {
		s              = std::clamp(s, kMinScale, kMaxScale);
		float const le = std::clamp(ref * s, kMinPx, kMaxPx); // longer edge in px
		if (aspect >= 1.0f) {
			size.x = le;
			size.y = le / aspect;
		} else {
			size.y = le;
			size.x = le * aspect;
		}
	};

	ImGui::TextUnformatted(title);

	// Derive the current scale from the longer edge so the slider reflects `size`.
	float scale   = std::max(size.x, size.y) / ref;
	bool  changed = false;

	ImGui::SetNextItemWidth(180.0f);
	if (ImGui::SliderFloat(drag_id, &scale, kMinScale, kMaxScale, "%.2fx")) {
		apply_scale(scale);
		changed = true;
	}

	// Quick multiplier presets.
	ImGui::PushID(drag_id);
	for (float const p : k_scale_presets) {
		ImGui::SameLine();
		char label[8];
		std::snprintf(label, sizeof(label), "%gx", p); // 0.5x / 1x / 2x / 3x
		bool const active = std::abs(scale - p) < 0.01f;
		if (active)
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		if (ImGui::SmallButton(label)) {
			apply_scale(p);
			changed = true;
		}
		if (active)
			ImGui::PopStyleColor();
	}
	ImGui::PopID();

	// Show the resulting pixel dimensions.
	ImGui::SameLine();
	ImGui::TextDisabled("%.0fx%.0f", size.x, size.y);

	return changed;
}
void ConfigRuntime::SetClearThumbnailCacheCallback(std::function<void()> cb) {
	m_on_clear_thumbnail_cache = std::move(cb);
}

void ConfigRuntime::SetClearFileExplorerCacheCallback(std::function<void()> cb) {
	m_on_clear_file_explorer_cache = std::move(cb);
}

void ConfigRuntime::SetClearVideoCacheCallback(std::function<void()> cb) {
	m_on_clear_video_cache = std::move(cb);
}

void ConfigRuntime::SetRebuildVideoCacheCallback(std::function<void()> cb) {
	m_on_rebuild_video_cache = std::move(cb);
}

void ConfigRuntime::SetClearHistoryMetadataCallback(std::function<void()> cb) {
	m_on_clear_history_metadata = std::move(cb);
}

void ConfigRuntime::SetDeleteAllCacheAndStateCallback(std::function<void()> cb) {
	m_on_delete_all_cache_and_state = std::move(cb);
}

void ConfigRuntime::SetReopenAppCallback(std::function<void()> cb) {
	m_on_reopen_app = std::move(cb);
}

void ConfigRuntime::SetVideoResumeThresholdChangedCallback(std::function<void(int)> cb) {
	m_on_video_resume_threshold_changed = std::move(cb);
}

void ConfigRuntime::SetHoverPreviewChangedCallback(std::function<void(bool, int)> cb) {
	m_on_hover_preview_changed = std::move(cb);
}

void ConfigRuntime::SetVideoPlaybackChangedCallback(std::function<void(int, bool)> cb) {
	m_on_video_playback_changed = std::move(cb);
}

void ConfigRuntime::SetVsyncChangedCallback(std::function<void(bool)> cb) {
	m_on_vsync_changed = std::move(cb);
}

bool ConfigRuntime::VsyncEnabled() const { return m_pending_vsync_enabled; }

std::string ConfigRuntime::ThumbnailFormat() const { return m_pending_thumbnail_format; }
std::string ConfigRuntime::ImageThumbnailTier() const { return m_pending_image_thumbnail_tier; }
std::string ConfigRuntime::VideoThumbnailTier() const { return m_pending_video_thumbnail_tier; }

void ConfigRuntime::SetVsyncEnabled(bool enabled) {
	m_pending_vsync_enabled = enabled;
	if (m_on_vsync_changed)
		m_on_vsync_changed(m_pending_vsync_enabled);
}

void ConfigRuntime::SetRestartAllThreadsCallback(std::function<void()> cb) {
	m_on_restart_all_threads = std::move(cb);
}

void ConfigRuntime::SetFileBrowserProvider(std::function<ImGui::FileBrowser*()> provider) {
	m_fb_provider = std::move(provider);
}

int ConfigRuntime::VideoResumeThresholdSeconds() const {
	return m_applied_video_resume_threshold_seconds;
}

float ConfigRuntime::HoldSpeedMultiplier() const { return m_pending_hold_speed_multiplier; }

int ConfigRuntime::SeekStepSeconds() const { return m_pending_seek_step_seconds; }

void ConfigRuntime::ApplyLayout(WindowStateToml const& state) {
	IsOpen = state.show_runtime_config_window;

	if (state.hover_preview_size) {
		VideoHoverPreview::preview_size
			= ImVec2 {state.hover_preview_size->x, state.hover_preview_size->y};
		m_pending_hover_size = VideoHoverPreview::preview_size;
	}
	if (state.seek_preview_size) {
		VideoSeekPreview::preview_size
			= ImVec2 {state.seek_preview_size->x, state.seek_preview_size->y};
		m_pending_seek_size = VideoSeekPreview::preview_size;
	}
	if (state.image_hover_preview_size) {
		ImageViewerPanel::hover_preview_size
			= ImVec2 {state.image_hover_preview_size->x, state.image_hover_preview_size->y};
	}

	m_pending_video_resume_threshold_seconds
		= std::max(state.video_resume_persist_min_duration_seconds, 0);
	m_applied_video_resume_threshold_seconds = m_pending_video_resume_threshold_seconds;
	if (m_on_video_resume_threshold_changed)
		m_on_video_resume_threshold_changed(m_applied_video_resume_threshold_seconds);

	m_pending_hold_speed_multiplier = std::clamp(state.video_hold_speed_multiplier, 1.0f, 8.0f);
	m_pending_seek_step_seconds     = std::clamp(state.video_seek_step_seconds, 1, 600);
	VideoUiWindow::hold_speed_multiplier = m_pending_hold_speed_multiplier;
	VideoUiWindow::seek_step_seconds     = m_pending_seek_step_seconds;

	m_pending_hover_preview_enabled  = state.hover_preview_enabled;
	m_pending_hover_preview_delay_ms = std::clamp(state.hover_preview_delay_ms, 0, 5000);
	m_pending_hover_preview_sound    = state.hover_preview_sound;
	VideoHoverPreview::enabled       = m_pending_hover_preview_enabled;
	VideoHoverPreview::hover_delay   = std::chrono::milliseconds(m_pending_hover_preview_delay_ms);
	VideoHoverPreview::preview_sound = m_pending_hover_preview_sound;
	if (m_on_hover_preview_changed)
		m_on_hover_preview_changed(VideoHoverPreview::enabled, m_pending_hover_preview_delay_ms);

	m_pending_global_playback_mode = (state.global_video_playback_mode >= 0)
		? sanitize_video_playback_mode(state.global_video_playback_mode)
		: (state.global_hwdec_enabled
				  ? static_cast<int>(VideoPlaybackMode::NvdecMpv)
				  : static_cast<int>(VideoPlaybackMode::SwMpv));
	m_pending_global_loop_enabled  = state.global_loop_enabled;
	if (m_on_video_playback_changed)
		m_on_video_playback_changed(m_pending_global_playback_mode, m_pending_global_loop_enabled);

	m_pending_vsync_enabled = state.vsync;
	if (m_on_vsync_changed)
		m_on_vsync_changed(m_pending_vsync_enabled);

	m_pending_thumbnail_format     = state.thumbnail_format;
	m_pending_image_thumbnail_tier = state.image_thumbnail_tier;
	m_pending_video_thumbnail_tier = state.video_thumbnail_tier;
}

void ConfigRuntime::ExportLayout(WindowStateToml* state) const {
	state->show_runtime_config_window = IsOpen;
	state->hover_preview_size         = WindowStateToml::Vec2Toml {
        VideoHoverPreview::preview_size.x, VideoHoverPreview::preview_size.y};
	state->seek_preview_size = WindowStateToml::Vec2Toml {
		VideoSeekPreview::preview_size.x, VideoSeekPreview::preview_size.y};
	state->image_hover_preview_size = WindowStateToml::Vec2Toml {
		ImageViewerPanel::hover_preview_size.x, ImageViewerPanel::hover_preview_size.y};
	state->video_resume_persist_min_duration_seconds = m_applied_video_resume_threshold_seconds;
	state->video_hold_speed_multiplier               = m_pending_hold_speed_multiplier;
	state->video_seek_step_seconds                   = m_pending_seek_step_seconds;
	state->hover_preview_enabled                     = VideoHoverPreview::enabled;
	state->hover_preview_delay_ms     = static_cast<int>(VideoHoverPreview::hover_delay.count());
	state->hover_preview_sound        = VideoHoverPreview::preview_sound;
	state->global_video_playback_mode = m_pending_global_playback_mode;
	state->global_hwdec_enabled       = mode_uses_hwdec(m_pending_global_playback_mode);
	state->global_loop_enabled        = m_pending_global_loop_enabled;
	state->vsync                      = m_pending_vsync_enabled;
	state->thumbnail_format           = m_pending_thumbnail_format;
	state->image_thumbnail_tier       = m_pending_image_thumbnail_tier;
	state->video_thumbnail_tier       = m_pending_video_thumbnail_tier;
}
