#include "config_runtime.hpp"

#include "video_hover_preview.hpp"
#include "video_seek_preview.hpp"

#include "imgui.h"

ConfigRuntime::ConfigRuntime()
    : IsOpen{false}
    , m_pending_hover_size{VideoHoverPreview::preview_size}
    , m_pending_seek_size{VideoSeekPreview::preview_size}
{
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

    ImGui::SetNextWindowSize(ImVec2(400.0f, 180.0f), ImGuiCond_FirstUseEver);
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

    ImGui::End();
}
