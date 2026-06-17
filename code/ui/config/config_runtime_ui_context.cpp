#include "pch.hpp"

#include "config_runtime_ui_context.hpp"

#include "Image_viewer_panel.hpp"
#include "app_context.hpp"
#include "config_runtime.hpp"
#include "video_hover_preview.hpp"
#include "video_playback_mode.hpp"
#include "video_seek_preview.hpp"
#include "video_ui_window.hpp"

void ConfigRuntimeUiContext::DrawUi(ConfigRuntime *cfg) {

	if (!cfg || !cfg->IsOpen)
		return;

	ImGui::SetNextWindowSize(ImVec2(400.0f, 380.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Runtime Config", &cfg->IsOpen)) {
		ImGui::End();
		return;
	}

	if (ImGui::CollapsingHeader("Image Preview Size", ImGuiTreeNodeFlags_DefaultOpen)) {
		cfg->DrawPreviewSizeControl("Image Hover Preview Size", "##image_hover_preview_size",
			ImageViewerPanel::hover_preview_size);
		ImGui::SameLine();
		if (ImGui::SmallButton("Original##img_orig")) {
			ImageViewerPanel::hover_preview_size = {280.0f, 180.0f};
		}
	}

	if (ImGui::CollapsingHeader("Video Preview Size", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (cfg->DrawPreviewSizeControl("Hover Preview Size", "##hover_preview_size", cfg->m_pending_hover_size))
			VideoHoverPreview::preview_size = cfg->m_pending_hover_size;
		{
			ImVec2 const src     = VideoHoverPreview::last_source_size;
			bool const   has_src = src.x > 0.0f && src.y > 0.0f;
			ImGui::SameLine();
			ImGui::BeginDisabled(!has_src);
			if (ImGui::SmallButton("Source size##hover_src_size")) {
				cfg->m_pending_hover_size       = src;
				VideoHoverPreview::preview_size = src;
			}
			ImGui::EndDisabled();
			if (has_src) {
				ImGui::SameLine();
				ImGui::TextDisabled("%.0fx%.0f", src.x, src.y);
			}
		}

		if (cfg->DrawPreviewSizeControl("Seek Preview Size", "##seek_preview_size", cfg->m_pending_seek_size))
			VideoSeekPreview::preview_size = cfg->m_pending_seek_size;
	}

	if (ImGui::CollapsingHeader("Hover Preview", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Checkbox("Enable hover preview", &cfg->m_pending_hover_preview_enabled)) {
			VideoHoverPreview::enabled = cfg->m_pending_hover_preview_enabled;
			if (cfg->m_on_hover_preview_changed)
				cfg->m_on_hover_preview_changed(cfg->m_pending_hover_preview_enabled,
					cfg->m_pending_hover_preview_delay_ms);
		}
		ImGui::SetNextItemWidth(200.0f);
		bool hover_changed = ImGui::DragInt("Dwell delay (ms)##hover_delay", &cfg->m_pending_hover_preview_delay_ms,
			10.0f, 0, 3000, "%d ms");
		ImGui::SameLine();
		ImGui::TextDisabled("Time cursor must rest before popup appears");
		if (hover_changed || ImGui::IsItemDeactivatedAfterEdit()) {
			cfg->m_pending_hover_preview_delay_ms = std::clamp(cfg->m_pending_hover_preview_delay_ms, 0, 3000);
			VideoHoverPreview::enabled            = cfg->m_pending_hover_preview_enabled;
			VideoHoverPreview::hover_delay        = std::chrono::milliseconds(cfg->m_pending_hover_preview_delay_ms);
			if (cfg->m_on_hover_preview_changed)
				cfg->m_on_hover_preview_changed(VideoHoverPreview::enabled, cfg->m_pending_hover_preview_delay_ms);
		}
		if (ImGui::Checkbox("Sound##hover_preview_sound", &cfg->m_pending_hover_preview_sound)) {
			VideoHoverPreview::preview_sound = cfg->m_pending_hover_preview_sound;
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Unmute audio in hover preview");
	}

	if (ImGui::CollapsingHeader("Video Playback", ImGuiTreeNodeFlags_DefaultOpen)) {
		bool playback_changed = false;

		int playback_mode = sanitize_video_playback_mode(cfg->m_pending_global_playback_mode);
		ImGui::SetNextItemWidth(180.0f);
		if (ImGui::Combo("Playback mode##global_playback_mode_combo", &playback_mode,
				k_video_playback_mode_items.data(), static_cast<int>(k_video_playback_mode_items.size()))) {
			cfg->m_pending_global_playback_mode = sanitize_video_playback_mode(playback_mode);
			playback_changed                    = true;
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Applies to all open videos instantly");

		if (ImGui::Checkbox("Loop##global_loop", &cfg->m_pending_global_loop_enabled)) {
			playback_changed = true;
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Applies to all open videos instantly");

		if (ImGui::Checkbox("VSync##global_vsync", &cfg->m_pending_vsync_enabled)) {
			if (cfg->m_on_vsync_changed)
				cfg->m_on_vsync_changed(cfg->m_pending_vsync_enabled);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Same behavior as Hello, world checkbox");

		if (playback_changed && cfg->m_on_video_playback_changed) {
			cfg->m_on_video_playback_changed(cfg->m_pending_global_playback_mode, cfg->m_pending_global_loop_enabled);
		}

		ImGui::Spacing();
		if (ImGui::Button("Restart All Threads")) {
			if (cfg->m_on_restart_all_threads)
				cfg->m_on_restart_all_threads();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Restart hover preview + reload all open videos");
	}

	if (ImGui::CollapsingHeader("Thumbnail Cache", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Button("Clear Thumbnail Cache")) {
			if (cfg->m_on_clear_thumbnail_cache)
				cfg->m_on_clear_thumbnail_cache();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Deletes cached PNG thumbnails on disk");
	}

	if (ImGui::CollapsingHeader("File Explorer Cache", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Button("Clear File Explorer Thumbnails")) {
			if (cfg->m_on_clear_file_explorer_cache)
				cfg->m_on_clear_file_explorer_cache();
		}
		ImGui::SameLine();
		ImGui::TextDisabled(
			"Stops generators, deletes on-disk PNGs and flushes "
			"the in-memory cache");
	}

	if (ImGui::CollapsingHeader("Video Cache", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Button("Clear Video Cache")) {
			if (cfg->m_on_clear_video_cache)
				cfg->m_on_clear_video_cache();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Deletes cached MP4 downloads on disk");

		if (ImGui::Button("Rebuild Video Cache")) {
			if (cfg->m_on_rebuild_video_cache)
				cfg->m_on_rebuild_video_cache();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Clears old cache and re-queues downloads from video history links");
	}

	if (ImGui::CollapsingHeader("Playback Persistence", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SetNextItemWidth(180.0f);
		if (ImGui::DragInt("Resume threshold (seconds)##resume_threshold",
				&cfg->m_pending_video_resume_threshold_seconds, 1.0f, 0, 600, "%d s")) {
			cfg->m_pending_video_resume_threshold_seconds
				= std::clamp(cfg->m_pending_video_resume_threshold_seconds, 0, 600);
			cfg->m_applied_video_resume_threshold_seconds = cfg->m_pending_video_resume_threshold_seconds;
			if (cfg->m_on_video_resume_threshold_changed)
				cfg->m_on_video_resume_threshold_changed(cfg->m_applied_video_resume_threshold_seconds);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Only videos at or above this duration save resume position");
	}

	if (ImGui::CollapsingHeader("Playback Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SetNextItemWidth(180.0f);
		if (ImGui::DragFloat("Hold-to-speed multiplier##hold_speed", &cfg->m_pending_hold_speed_multiplier, 0.05f, 1.0f,
				8.0f, "%.2fx")) {
			cfg->m_pending_hold_speed_multiplier = std::clamp(cfg->m_pending_hold_speed_multiplier, 1.0f, 8.0f);
			VideoUiWindow::hold_speed_multiplier = cfg->m_pending_hold_speed_multiplier;
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Speed while holding left mouse on the video");

		ImGui::SetNextItemWidth(180.0f);
		if (ImGui::DragInt("Seek step (seconds)##seek_step", &cfg->m_pending_seek_step_seconds, 1.0f, 1, 600, "%d s")) {
			cfg->m_pending_seek_step_seconds = std::clamp(cfg->m_pending_seek_step_seconds, 1, 600);
			VideoUiWindow::seek_step_seconds = cfg->m_pending_seek_step_seconds;
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Left/Right arrow keys and seek buttons jump this many seconds");
	}

	if (ImGui::CollapsingHeader("History Metadata", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Button("Clear History Metadata")) {
			if (cfg->m_on_clear_history_metadata)
				cfg->m_on_clear_history_metadata();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Clears history entries from window_state.toml");

		if (ImGui::Button("Delete All Cache + Erase TOML")) {
			if (cfg->m_on_delete_all_cache_and_state)
				cfg->m_on_delete_all_cache_and_state();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Deletes thumbnail/video cache and removes window_state.toml");
	}

	if (ImGui::CollapsingHeader("Application", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Button("Reopen App")) {
			if (cfg->m_on_reopen_app)
				cfg->m_on_reopen_app();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Closes and restarts the app immediately");
	}

	ImGui::End();
}
