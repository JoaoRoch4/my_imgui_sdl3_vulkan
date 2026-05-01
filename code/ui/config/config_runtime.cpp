#include "config_runtime.hpp"

#include "video_hover_preview.hpp"
#include "video_seek_preview.hpp"

#include "imgui.h"

ConfigRuntime::ConfigRuntime()
    : IsOpen{false}
    , m_pending_hover_size{VideoHoverPreview::preview_size}
    , m_pending_seek_size{VideoSeekPreview::preview_size}
    , m_on_clear_thumbnail_cache{nullptr}
    , m_on_clear_video_cache{nullptr}
    , m_on_rebuild_video_cache{nullptr}
    , m_on_clear_history_metadata{nullptr}
{
}

void ConfigRuntime::SetClearThumbnailCacheCallback(std::function<void()> cb)
{
    m_on_clear_thumbnail_cache = std::move(cb);
}

void ConfigRuntime::SetClearVideoCacheCallback(std::function<void()> cb)
{
    m_on_clear_video_cache = std::move(cb);
}

void ConfigRuntime::SetRebuildVideoCacheCallback(std::function<void()> cb)
{
    m_on_rebuild_video_cache = std::move(cb);
}

void ConfigRuntime::SetClearHistoryMetadataCallback(std::function<void()> cb)
{
    m_on_clear_history_metadata = std::move(cb);
}

void ConfigRuntime::ApplyLayout(const WindowStateToml &state)
{
    if (state.hover_preview_size) {
        VideoHoverPreview::preview_size = ImVec2{state.hover_preview_size->x, state.hover_preview_size->y};
        m_pending_hover_size = VideoHoverPreview::preview_size;
    }
    if (state.seek_preview_size) {
        VideoSeekPreview::preview_size = ImVec2{state.seek_preview_size->x, state.seek_preview_size->y};
        m_pending_seek_size = VideoSeekPreview::preview_size;
    }
}

void ConfigRuntime::ExportLayout(WindowStateToml *state) const
{
    state->hover_preview_size = WindowStateToml::Vec2Toml{VideoHoverPreview::preview_size.x, VideoHoverPreview::preview_size.y};
    state->seek_preview_size  = WindowStateToml::Vec2Toml{VideoSeekPreview::preview_size.x,  VideoSeekPreview::preview_size.y};
}

void ConfigRuntime::Draw()
{
    if (!IsOpen)
        return;

    ImGui::SetNextWindowSize(ImVec2(400.0f, 380.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Runtime Config", &IsOpen))
    {
        ImGui::End();
        return;
    }

    if (ImGui::CollapsingHeader("Video Preview Size", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat2("Hover preview##pending", &m_pending_hover_size.x,
                            80.0f, 1920.0f, "%.0f px");
        ImGui::SliderFloat2("Seek preview##pending",  &m_pending_seek_size.x,
                            80.0f, 1920.0f, "%.0f px");

        ImGui::Spacing();
        if (ImGui::Button("Apply"))
        {
            VideoHoverPreview::preview_size = m_pending_hover_size;
            VideoSeekPreview::preview_size  = m_pending_seek_size;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(takes effect for the next video opened)");
    }

    if (ImGui::CollapsingHeader("Thumbnail Cache", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::Button("Clear Thumbnail Cache"))
        {
            if (m_on_clear_thumbnail_cache)
                m_on_clear_thumbnail_cache();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Deletes cached PNG thumbnails on disk");
    }

    if (ImGui::CollapsingHeader("Video Cache", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::Button("Clear Video Cache"))
        {
            if (m_on_clear_video_cache)
                m_on_clear_video_cache();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Deletes cached MP4 downloads on disk");

        if (ImGui::Button("Rebuild Video Cache"))
        {
            if (m_on_rebuild_video_cache)
                m_on_rebuild_video_cache();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Clears old cache and re-queues downloads from video history links");
    }

    if (ImGui::CollapsingHeader("History Metadata", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::Button("Clear History Metadata"))
        {
            if (m_on_clear_history_metadata)
                m_on_clear_history_metadata();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Clears history entries from window_state.toml");
    }

    ImGui::End();
}
