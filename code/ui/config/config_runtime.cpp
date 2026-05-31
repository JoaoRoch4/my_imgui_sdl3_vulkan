#include "pch.hpp"

#include "config_runtime.hpp"

#include "Image_viewer_panel.hpp"
#include "video_hover_preview.hpp"
#include "video_playback_mode.hpp"
#include "video_seek_preview.hpp"

#include <algorithm>

namespace {

 constexpr struct {
  float w, h;
  const char *label;
} k_size_presets[] = {
    {640, 480, "480"},
    {800, 600, "600"},
    {1280, 800, "800"},
};

// Returns true if the value changed.
bool DrawPreviewSizeControl(const char *title, const char *drag_id,
                            ImVec2 &size) {
  ImGui::TextUnformatted(title);
  ImGui::SetNextItemWidth(180.0f);
  bool changed =
      ImGui::DragFloat2(drag_id, &size.x, 1.0f, 80.0f, 1920.0f, "%.0f px");
  if (changed) {
    size.x = std::clamp(size.x, 80.0f, 1920.0f);
    size.y = std::clamp(size.y, 80.0f, 1920.0f);
  }
  for (const auto &p : k_size_presets) {
    ImGui::SameLine();
    // Highlight if this preset is currently active
    const bool active = (size.x == p.w && size.y == p.h);
    if (active)
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::PushID(drag_id);
    if (ImGui::SmallButton(p.label)) {
      size = {p.w, p.h};
      changed = true;
    }
    ImGui::PopID();
    if (active)
      ImGui::PopStyleColor();
  }
  return changed;
}

} // namespace

ConfigRuntime::ConfigRuntime()
    : IsOpen{false}, m_pending_hover_size{VideoHoverPreview::preview_size},
      m_pending_seek_size{VideoSeekPreview::preview_size},
      m_pending_video_resume_threshold_seconds{
          WindowStateToml{}.video_resume_persist_min_duration_seconds},
      m_applied_video_resume_threshold_seconds{
          WindowStateToml{}.video_resume_persist_min_duration_seconds},
      m_on_clear_thumbnail_cache{nullptr},
      m_on_clear_file_explorer_cache{nullptr},
      m_on_clear_video_cache{nullptr},
      m_on_rebuild_video_cache{nullptr}, m_on_clear_history_metadata{nullptr},
      m_on_delete_all_cache_and_state{nullptr}, m_on_reopen_app{nullptr},
      m_on_video_resume_threshold_changed{nullptr},
      m_pending_hover_preview_enabled{VideoHoverPreview::enabled},
      m_pending_hover_preview_delay_ms{
          static_cast<int>(VideoHoverPreview::hover_delay.count())},
      m_pending_hover_preview_sound{VideoHoverPreview::preview_sound},
      m_on_hover_preview_changed{nullptr},
      m_pending_global_playback_mode{
          static_cast<int>(VideoPlaybackMode::SwMpv)},
      m_pending_global_loop_enabled{false},
      m_on_video_playback_changed{nullptr},
      m_pending_vsync_enabled{WindowStateToml{}.vsync},
      m_on_vsync_changed{nullptr}, m_on_restart_all_threads{nullptr} {}

void ConfigRuntime::SetClearThumbnailCacheCallback(std::function<void()> cb) {
  m_on_clear_thumbnail_cache = std::move(cb);
}

void ConfigRuntime::SetClearFileExplorerCacheCallback(
    std::function<void()> cb) {
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

void ConfigRuntime::SetDeleteAllCacheAndStateCallback(
    std::function<void()> cb) {
  m_on_delete_all_cache_and_state = std::move(cb);
}

void ConfigRuntime::SetReopenAppCallback(std::function<void()> cb) {
  m_on_reopen_app = std::move(cb);
}

void ConfigRuntime::SetVideoResumeThresholdChangedCallback(
    std::function<void(int)> cb) {
  m_on_video_resume_threshold_changed = std::move(cb);
}

void ConfigRuntime::SetHoverPreviewChangedCallback(
    std::function<void(bool, int)> cb) {
  m_on_hover_preview_changed = std::move(cb);
}

void ConfigRuntime::SetVideoPlaybackChangedCallback(
    std::function<void(int, bool)> cb) {
  m_on_video_playback_changed = std::move(cb);
}

void ConfigRuntime::SetVsyncChangedCallback(std::function<void(bool)> cb) {
  m_on_vsync_changed = std::move(cb);
}

bool ConfigRuntime::VsyncEnabled() const {
  return m_pending_vsync_enabled;
}

void ConfigRuntime::SetVsyncEnabled(bool enabled) {
  m_pending_vsync_enabled = enabled;
  if (m_on_vsync_changed)
    m_on_vsync_changed(m_pending_vsync_enabled);
}

void ConfigRuntime::SetRestartAllThreadsCallback(std::function<void()> cb) {
  m_on_restart_all_threads = std::move(cb);
}

int ConfigRuntime::VideoResumeThresholdSeconds() const {
  return m_applied_video_resume_threshold_seconds;
}

void ConfigRuntime::ApplyLayout(const WindowStateToml &state) {
  IsOpen = state.show_runtime_config_window;

  if (state.hover_preview_size) {
    VideoHoverPreview::preview_size =
        ImVec2{state.hover_preview_size->x, state.hover_preview_size->y};
    m_pending_hover_size = VideoHoverPreview::preview_size;
  }
  if (state.seek_preview_size) {
    VideoSeekPreview::preview_size =
        ImVec2{state.seek_preview_size->x, state.seek_preview_size->y};
    m_pending_seek_size = VideoSeekPreview::preview_size;
  }
  if (state.image_hover_preview_size) {
    ImageViewerPanel::hover_preview_size =
        ImVec2{state.image_hover_preview_size->x,
               state.image_hover_preview_size->y};
  }

  m_pending_video_resume_threshold_seconds =
      std::max(state.video_resume_persist_min_duration_seconds, 0);
  m_applied_video_resume_threshold_seconds =
      m_pending_video_resume_threshold_seconds;
  if (m_on_video_resume_threshold_changed)
    m_on_video_resume_threshold_changed(
        m_applied_video_resume_threshold_seconds);

  m_pending_hover_preview_enabled = state.hover_preview_enabled;
  m_pending_hover_preview_delay_ms =
      std::clamp(state.hover_preview_delay_ms, 0, 5000);
  m_pending_hover_preview_sound = state.hover_preview_sound;
  VideoHoverPreview::enabled = m_pending_hover_preview_enabled;
  VideoHoverPreview::hover_delay =
      std::chrono::milliseconds(m_pending_hover_preview_delay_ms);
  VideoHoverPreview::preview_sound = m_pending_hover_preview_sound;
  if (m_on_hover_preview_changed)
    m_on_hover_preview_changed(VideoHoverPreview::enabled,
                               m_pending_hover_preview_delay_ms);

  m_pending_global_playback_mode =
      (state.global_video_playback_mode >= 0)
          ? sanitize_video_playback_mode(state.global_video_playback_mode)
          : (state.global_hwdec_enabled
                 ? static_cast<int>(VideoPlaybackMode::NvdecMpv)
                 : static_cast<int>(VideoPlaybackMode::SwMpv));
  m_pending_global_loop_enabled = state.global_loop_enabled;
  if (m_on_video_playback_changed)
    m_on_video_playback_changed(m_pending_global_playback_mode,
                                m_pending_global_loop_enabled);

  m_pending_vsync_enabled = state.vsync;
  if (m_on_vsync_changed)
    m_on_vsync_changed(m_pending_vsync_enabled);
}

void ConfigRuntime::ExportLayout(WindowStateToml *state) const {
  state->show_runtime_config_window = IsOpen;
  state->hover_preview_size = WindowStateToml::Vec2Toml{
      VideoHoverPreview::preview_size.x, VideoHoverPreview::preview_size.y};
  state->seek_preview_size = WindowStateToml::Vec2Toml{
      VideoSeekPreview::preview_size.x, VideoSeekPreview::preview_size.y};
  state->image_hover_preview_size = WindowStateToml::Vec2Toml{
      ImageViewerPanel::hover_preview_size.x,
      ImageViewerPanel::hover_preview_size.y};
  state->video_resume_persist_min_duration_seconds =
      m_applied_video_resume_threshold_seconds;
  state->hover_preview_enabled = VideoHoverPreview::enabled;
  state->hover_preview_delay_ms =
      static_cast<int>(VideoHoverPreview::hover_delay.count());
  state->hover_preview_sound = VideoHoverPreview::preview_sound;
  state->global_video_playback_mode = m_pending_global_playback_mode;
  state->global_hwdec_enabled = mode_uses_hwdec(m_pending_global_playback_mode);
  state->global_loop_enabled = m_pending_global_loop_enabled;
  state->vsync = m_pending_vsync_enabled;
}

void ConfigRuntime::Draw() {
  if (!IsOpen)
    return;

  ImGui::SetNextWindowSize(ImVec2(400.0f, 380.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Runtime Config", &IsOpen)) {
    ImGui::End();
    return;
  }

  if (ImGui::CollapsingHeader("Image Preview Size",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    DrawPreviewSizeControl("Image Hover Preview Size",
                           "##image_hover_preview_size",
                           ImageViewerPanel::hover_preview_size);
    ImGui::SameLine();
    if (ImGui::SmallButton("Original##img_orig")) {
      ImageViewerPanel::hover_preview_size = {280.0f, 180.0f};
    }
  }

  if (ImGui::CollapsingHeader("Video Preview Size",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    if (DrawPreviewSizeControl("Hover Preview Size", "##hover_preview_size",
                               m_pending_hover_size))
      VideoHoverPreview::preview_size = m_pending_hover_size;
    {
      const ImVec2 src = VideoHoverPreview::last_source_size;
      const bool has_src = src.x > 0.0f && src.y > 0.0f;
      ImGui::SameLine();
      ImGui::BeginDisabled(!has_src);
      if (ImGui::SmallButton("Source size##hover_src_size")) {
        m_pending_hover_size = src;
        VideoHoverPreview::preview_size = src;
      }
      ImGui::EndDisabled();
      if (has_src) {
        ImGui::SameLine();
        ImGui::TextDisabled("%.0fx%.0f", src.x, src.y);
      }
    }

    if (DrawPreviewSizeControl("Seek Preview Size", "##seek_preview_size",
                               m_pending_seek_size))
      VideoSeekPreview::preview_size = m_pending_seek_size;
  }

  if (ImGui::CollapsingHeader("Hover Preview",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::Checkbox("Enable hover preview",
                        &m_pending_hover_preview_enabled)) {
      VideoHoverPreview::enabled = m_pending_hover_preview_enabled;
      if (m_on_hover_preview_changed)
        m_on_hover_preview_changed(m_pending_hover_preview_enabled,
                                   m_pending_hover_preview_delay_ms);
    }
    ImGui::SetNextItemWidth(200.0f);
    bool hover_changed = ImGui::DragInt("Dwell delay (ms)##hover_delay",
                                        &m_pending_hover_preview_delay_ms,
                                        10.0f, 0, 3000, "%d ms");
    ImGui::SameLine();
    ImGui::TextDisabled("Time cursor must rest before popup appears");
    if (hover_changed || ImGui::IsItemDeactivatedAfterEdit()) {
      m_pending_hover_preview_delay_ms =
          std::clamp(m_pending_hover_preview_delay_ms, 0, 3000);
      VideoHoverPreview::enabled = m_pending_hover_preview_enabled;
      VideoHoverPreview::hover_delay =
          std::chrono::milliseconds(m_pending_hover_preview_delay_ms);
      if (m_on_hover_preview_changed)
        m_on_hover_preview_changed(VideoHoverPreview::enabled,
                                   m_pending_hover_preview_delay_ms);
    }
    if (ImGui::Checkbox("Sound##hover_preview_sound",
                        &m_pending_hover_preview_sound)) {
      VideoHoverPreview::preview_sound = m_pending_hover_preview_sound;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Unmute audio in hover preview");
  }

  if (ImGui::CollapsingHeader("Video Playback",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    bool playback_changed = false;

    int playback_mode =
        sanitize_video_playback_mode(m_pending_global_playback_mode);
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::Combo("Playback mode##global_playback_mode_combo",
                     &playback_mode, k_video_playback_mode_items.data(),
                     static_cast<int>(k_video_playback_mode_items.size()))) {
      m_pending_global_playback_mode =
          sanitize_video_playback_mode(playback_mode);
      playback_changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Applies to all open videos instantly");

    if (ImGui::Checkbox("Loop##global_loop", &m_pending_global_loop_enabled)) {
      playback_changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Applies to all open videos instantly");

    if (ImGui::Checkbox("VSync##global_vsync", &m_pending_vsync_enabled)) {
      if (m_on_vsync_changed)
        m_on_vsync_changed(m_pending_vsync_enabled);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Same behavior as Hello, world checkbox");

    if (playback_changed && m_on_video_playback_changed) {
      m_on_video_playback_changed(m_pending_global_playback_mode,
                                  m_pending_global_loop_enabled);
    }

    ImGui::Spacing();
    if (ImGui::Button("Restart All Threads")) {
      if (m_on_restart_all_threads)
        m_on_restart_all_threads();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Restart hover preview + reload all open videos");
  }

  if (ImGui::CollapsingHeader("Thumbnail Cache",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::Button("Clear Thumbnail Cache")) {
      if (m_on_clear_thumbnail_cache)
        m_on_clear_thumbnail_cache();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Deletes cached PNG thumbnails on disk");
  }

  if (ImGui::CollapsingHeader("File Explorer Cache",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::Button("Clear File Explorer Thumbnails")) {
      if (m_on_clear_file_explorer_cache)
        m_on_clear_file_explorer_cache();
    }
    ImGui::SameLine();
    ImGui::TextDisabled(
        "Stops generators, deletes on-disk PNGs and flushes the in-memory cache");
  }

  if (ImGui::CollapsingHeader("Video Cache", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::Button("Clear Video Cache")) {
      if (m_on_clear_video_cache)
        m_on_clear_video_cache();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Deletes cached MP4 downloads on disk");

    if (ImGui::Button("Rebuild Video Cache")) {
      if (m_on_rebuild_video_cache)
        m_on_rebuild_video_cache();
    }
    ImGui::SameLine();
    ImGui::TextDisabled(
        "Clears old cache and re-queues downloads from video history links");
  }

  if (ImGui::CollapsingHeader("Playback Persistence",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::DragInt("Resume threshold (seconds)##resume_threshold",
                       &m_pending_video_resume_threshold_seconds, 1.0f, 0, 600,
                       "%d s")) {
      m_pending_video_resume_threshold_seconds =
          std::clamp(m_pending_video_resume_threshold_seconds, 0, 600);
      m_applied_video_resume_threshold_seconds =
          m_pending_video_resume_threshold_seconds;
      if (m_on_video_resume_threshold_changed)
        m_on_video_resume_threshold_changed(
            m_applied_video_resume_threshold_seconds);
    }
    ImGui::SameLine();
    ImGui::TextDisabled(
        "Only videos at or above this duration save resume position");
  }

  if (ImGui::CollapsingHeader("History Metadata",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::Button("Clear History Metadata")) {
      if (m_on_clear_history_metadata)
        m_on_clear_history_metadata();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Clears history entries from window_state.toml");

    if (ImGui::Button("Delete All Cache + Erase TOML")) {
      if (m_on_delete_all_cache_and_state)
        m_on_delete_all_cache_and_state();
    }
    ImGui::SameLine();
    ImGui::TextDisabled(
        "Deletes thumbnail/video cache and removes window_state.toml");
  }

  if (ImGui::CollapsingHeader("Application", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::Button("Reopen App")) {
      if (m_on_reopen_app)
        m_on_reopen_app();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Closes and restarts the app immediately");
  }

  ImGui::End();
}
