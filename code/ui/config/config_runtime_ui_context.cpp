#include "pch.hpp"

#include "config_runtime_ui_context.hpp"

#include "Image_viewer_panel.hpp"
#include "config_runtime.hpp"
#include "file_browser_ui.hpp"
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

		// Thumbnail storage backend. Applied at file-browser setup, so a change
		// takes effect on the next run (or after clearing the thumbnail cache).
		{
			constexpr std::array<char const*, 2> k_formats = {"bc1", "png"};
			int current = (cfg->m_pending_thumbnail_format == "png") ? 1 : 0;
			if (ImGui::Combo("Thumbnail format##thumb_format", &current, k_formats.data(),
					static_cast<int>(k_formats.size()))) {
				cfg->m_pending_thumbnail_format = k_formats[static_cast<std::size_t>(current)];
			}
			ImGui::SameLine();
			ImGui::TextDisabled("bc1 = GPU block-compressed (~8x smaller); applies next run");

			// Per-type quality presets. Image tiers cap the (lossless RGBA) decode resolution;
			// video tiers set the BC1 letterbox size. Both apply at file-browser setup (next run).
			constexpr std::array<char const*, 4> k_tiers = {"original", "high", "medium", "low"};
			auto tier_combo = [&](char const* label, std::string& pending, char const* help) {
				int idx = 0;
				for (int i = 0; i < static_cast<int>(k_tiers.size()); ++i)
					if (pending == k_tiers[static_cast<std::size_t>(i)]) {
						idx = i;
						break;
					}
				if (ImGui::Combo(label, &idx, k_tiers.data(), static_cast<int>(k_tiers.size())))
					pending = k_tiers[static_cast<std::size_t>(idx)];
				ImGui::SameLine();
				ImGui::TextDisabled("%s", help);
			};
			tier_combo("Image quality##img_tier", cfg->m_pending_image_thumbnail_tier,
				"resolution cap (lossless RGBA); applies next run");
			tier_combo("Video quality##vid_tier", cfg->m_pending_video_thumbnail_tier,
				"BC1 letterbox size; applies next run");
		}

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

	// File Explorer thumbnail sizes — three knobs, one per ViewMode. The provider
	// returns null while the explorer is closed; we still draw the section but
	// disable the controls so users see the feature exists.
	if (ImGui::CollapsingHeader("File Explorer Thumbnail Size", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::FileBrowser* fb = cfg->m_fb_provider ? cfg->m_fb_provider() : nullptr;
		ImGui::BeginDisabled(fb == nullptr);

		// Cache reads behind the disable so the disabled-state UI still has sane values.
		ImVec2 listSz    = fb ? fb->GetThumbnailSize()        : ImVec2 {64.0f, 36.0f};
		ImVec2 gridSz    = fb ? fb->GetGridThumbnailSize()    : ImVec2 {160.0f, 90.0f};
		ImVec2 masonrySz = fb ? fb->GetMasonryThumbnailSize() : ImVec2 {200.0f, 200.0f};

		ImGui::SetNextItemWidth(220.0f);
		if (ImGui::DragFloat2("List thumb (w, h)##fe_list_thumb", &listSz.x, 1.0f, 24.0f, 256.0f, "%.0f px") && fb) {
			listSz.x = std::clamp(listSz.x, 24.0f, 256.0f);
			listSz.y = std::clamp(listSz.y, 24.0f, 256.0f);
			fb->SetThumbnailSize(listSz);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Inline rows in list view");

		ImGui::SetNextItemWidth(220.0f);
		if (ImGui::DragFloat2("Grid thumb (w, h)##fe_grid_thumb", &gridSz.x, 1.0f, 64.0f, 1024.0f, "%.0f px") && fb) {
			gridSz.x = std::clamp(gridSz.x, 64.0f, 1024.0f);
			gridSz.y = std::clamp(gridSz.y, 64.0f, 1024.0f);
			fb->SetGridThumbnailSize(gridSz);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Uniform cells in grid view");

		ImGui::SetNextItemWidth(160.0f);
		if (ImGui::DragFloat("Masonry column width##fe_masonry_col", &masonrySz.x, 1.0f, 80.0f, 480.0f, "%.0f px") && fb) {
			masonrySz.x = std::clamp(masonrySz.x, 80.0f, 480.0f);
			fb->SetMasonryThumbnailSize(masonrySz);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("Used as a max column-width hint when columns = 0");

		int masonryCols = fb ? fb->GetMasonryColumns() : 0;
		ImGui::SetNextItemWidth(160.0f);
		// SliderInt with format string so 0 shows as "auto" instead of "0".
		char const* col_fmt = (masonryCols == 0) ? "auto" : "%d";
		if (ImGui::SliderInt("Masonry columns##fe_masonry_cols", &masonryCols, 0, 12, col_fmt) && fb) {
			masonryCols = std::clamp(masonryCols, 0, 12);
			fb->SetMasonryColumns(masonryCols);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("0 = auto (driven by column-width hint above)");

		int scrollStep = fb ? fb->GetScrollStep() : 40;
		ImGui::SetNextItemWidth(160.0f);
		if (ImGui::SliderInt("Keyboard scroll step##fe_scroll_step", &scrollStep, 4, 400, "%d px") && fb) {
			fb->SetScrollStep(scrollStep);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("W/S scroll amount (Shift+W/S pages)");

		ImGui::EndDisabled();
		if (fb == nullptr)
			ImGui::TextDisabled("Open the File Explorer to edit these.");
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
