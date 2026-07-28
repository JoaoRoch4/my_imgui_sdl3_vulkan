#include "pch.hpp" // NOLINT

#include "scene_strip.hpp"

#include "frame_pattern.hpp"
#include "motion.hpp"
#include "playback_clock.hpp"

void SceneStrip::sync(const PlaybackClock &clock)
{
    if (clock.seed() == m_built_seed && clock.duration() == m_built_duration &&
        step_seconds == m_built_step)
        return;

    m_built_seed     = clock.seed();
    m_built_duration = clock.duration();
    m_built_step     = step_seconds;

    m_frames.clear();
    if (clock.duration() <= 0.0 || step_seconds <= 0.0f)
        return;

    // Labels are formatted once here rather than every frame in the draw loop — the
    // strip can be hundreds of tiles on a long clip.
    const int count = static_cast<int>(clock.duration() / static_cast<double>(step_seconds)) + 1;
    m_frames.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double ts = static_cast<double>(i) * static_cast<double>(step_seconds);
        if (ts > clock.duration())
            break;
        m_frames.push_back({ts, PlaybackClock::format_time(ts)});
    }
}

double SceneStrip::draw(const PlaybackClock &clock, float dt)
{
    double clicked = -1.0;

    ImGui::Checkbox("Follow", &m_follow_playhead);
    ImGui::SameLine();
    ImGui::TextDisabled("%d frames", frame_count());

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##step", &step_seconds, 1.0f, 30.0f, "+%.0f sec");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Seconds between scene frames");

    if (m_frames.empty()) {
        ImGui::TextDisabled("No clip loaded");
        return clicked;
    }

    // Which tile the playhead is inside — highlighted, and scrolled to when following.
    const int current = std::clamp(
        static_cast<int>(clock.position() / static_cast<double>(step_seconds)), 0,
        frame_count() - 1);

    if (!ImGui::BeginChild("##scene_scroll", {0.0f, 0.0f}, ImGuiChildFlags_None,
                           ImGuiWindowFlags_NoBackground)) {
        ImGui::EndChild();
        return clicked;
    }

    const float tile_w = std::max(ImGui::GetContentRegionAvail().x, 32.0f);
    const float tile_h = tile_w * 9.0f / 16.0f;
    ImDrawList *dl     = ImGui::GetWindowDrawList();

    for (int i = 0; i < frame_count(); ++i) {
        const SceneFrame &frame = m_frames[static_cast<std::size_t>(i)];
        const bool        is_current = (i == current);

        ImGui::PushID(i);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        // Reserve the row first, then draw into it — hover state must be known before
        // the frame is drawn so the grow can be applied the same frame.
        ImGui::InvisibleButton("##tile", {tile_w, tile_h + ImGui::GetTextLineHeight() + 6.0f});
        const bool hovered = ImGui::IsItemHovered();
        const bool pressed = ImGui::IsItemClicked();

        // GetItemID after the item: stable per tile, and already scoped by the PushID,
        // so two tiles never share a Motion channel.
        const ImGuiID tile_id = ImGui::GetItemID();

        // Hover grows the tile a little and lifts it toward the panel edge; the
        // current tile sits permanently slightly proud so the playhead is findable
        // without reading labels.
        const float grow = Motion::tween_float(tile_id, ImHashStr("grow"),
                                               hovered ? 1.0f : (is_current ? 0.45f : 0.0f),
                                               0.16f, EasePreset::OutCubic, dt, 0.0f);
        const float inset = 6.0f * (1.0f - grow);

        const PanelBounds pic{{origin.x + inset, origin.y + inset * 0.5f},
                              {origin.x + tile_w - inset, origin.y + tile_h - inset * 0.5f}};

        FramePattern::draw(dl, pic, clock.seed(), frame.timestamp);

        // Accent ring: the clip's key colour, opacity driven by the same grow channel.
        const ImU32 ring = is_current ? FramePattern::key_color(clock.seed(), 0.95f)
                                      : ImGui::GetColorU32(ImVec4(1, 1, 1, 0.10f + 0.45f * grow));
        dl->AddRect(pic.min, pic.max, ring, 4.0f, 0, is_current ? 2.5f : 1.0f);

        dl->AddText({origin.x + 4.0f, origin.y + tile_h + 2.0f},
                    ImGui::GetColorU32(is_current ? ImVec4(1, 1, 1, 1) : ImVec4(1, 1, 1, 0.55f)),
                    frame.label.c_str());

        if (pressed)
            clicked = frame.timestamp;

        if (m_follow_playhead && is_current && !ImGui::IsAnyItemActive())
            ImGui::SetScrollHereY(0.5f);

        ImGui::PopID();
    }

    ImGui::EndChild();
    return clicked;
}
