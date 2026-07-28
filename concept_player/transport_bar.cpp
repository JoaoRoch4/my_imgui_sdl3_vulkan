#include "pch.hpp" // NOLINT

#include "transport_bar.hpp"

#include "frame_pattern.hpp"
#include "motion.hpp"
#include "playback_clock.hpp"
#include "video_stage.hpp"

int TransportBar::draw(PlaybackClock &clock, VideoStage &stage, bool &fullscreen, float dt)
{
    int         switch_relative = 0;
    ImDrawList *dl              = ImGui::GetWindowDrawList();

    // ── Seek strip ───────────────────────────────────────────────────────────
    // Full width, its own row. The hit area is deliberately much taller than the drawn
    // strip: a 4 px bar is unclickable, but a 4 px bar with a 20 px hit box feels
    // precise. This is the pattern the imanim skill calls the seek-bar recipe.
    constexpr float k_hit_height = 22.0f;
    const ImVec2    strip_origin = ImGui::GetCursorScreenPos();
    const float     strip_w      = std::max(ImGui::GetContentRegionAvail().x, 1.0f);

    ImGui::InvisibleButton("##seek", {strip_w, k_hit_height});
    const bool    strip_hovered = ImGui::IsItemHovered();
    const bool    strip_held    = ImGui::IsItemActive();
    const ImGuiID strip_id      = ImGui::GetItemID();

    const float frac_from_mouse =
        std::clamp((ImGui::GetIO().MousePos.x - strip_origin.x) / strip_w, 0.0f, 1.0f);

    if (strip_held) {
        m_scrubbing      = true;
        m_scrub_fraction = frac_from_mouse;
        clock.seek_to(static_cast<double>(m_scrub_fraction) * clock.duration());
    } else {
        m_scrubbing = false;
    }

    const bool  strip_active = strip_hovered || strip_held;
    const float thickness =
        Motion::tween_float(strip_id, ImHashStr("thick"),
                            strip_active ? bar_thickness_hover : bar_thickness_idle, 0.18f,
                            EasePreset::OutCubic, dt, bar_thickness_idle);
    const float knob = Motion::tween_toggle(strip_id, ImHashStr("knob"), strip_active, 0.18f,
                                            EasePreset::OutBack, dt, /*init_on=*/false);

    const float  strip_cy = strip_origin.y + k_hit_height * 0.5f;
    const ImVec2 track_min{strip_origin.x, strip_cy - thickness * 0.5f};
    const ImVec2 track_max{strip_origin.x + strip_w, strip_cy + thickness * 0.5f};

    const float played_frac = m_scrubbing ? m_scrub_fraction : clock.fraction();
    const float played_x    = strip_origin.x + strip_w * played_frac;

    dl->AddRectFilled(track_min, track_max, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.16f)),
                      thickness * 0.5f);
    // The played portion in the sketch's accent red — the one saturated colour in the
    // whole chrome, so the eye finds the playhead instantly against any footage.
    dl->AddRectFilled(track_min, {played_x, track_max.y},
                      ImGui::GetColorU32(ImVec4(0.91f, 0.24f, 0.28f, 0.95f)), thickness * 0.5f);

    if (knob > 0.002f)
        dl->AddCircleFilled({played_x, strip_cy}, (thickness * 0.5f + 4.0f) * knob,
                            ImGui::GetColorU32(ImVec4(1, 1, 1, 0.95f)));

    // ── Seek preview ─────────────────────────────────────────────────────────
    // Hovering anywhere on the strip shows the frame at that timestamp above it. The
    // real player already does this with a private mpv instance (VideoSeekPreview);
    // here the same interaction costs a draw call.
    if (strip_active && clock.duration() > 0.0) {
        const double at      = static_cast<double>(frac_from_mouse) * clock.duration();
        const float  prev_w  = 208.0f;
        const float  prev_h  = prev_w * 9.0f / 16.0f;
        const float  label_h = ImGui::GetTextLineHeight() + 6.0f;

        const float px = std::clamp(strip_origin.x + strip_w * frac_from_mouse - prev_w * 0.5f,
                                    strip_origin.x, strip_origin.x + strip_w - prev_w);
        const PanelBounds card{{px, track_min.y - prev_h - label_h - 12.0f},
                               {px + prev_w, track_min.y - 12.0f}};

        ImDrawList *fg = ImGui::GetForegroundDrawList();
        fg->AddRectFilled(card.min, card.max,
                          ImGui::GetColorU32(ImVec4(0.05f, 0.05f, 0.07f, 0.96f)), 8.0f);
        const PanelBounds pic{{card.min.x + 4.0f, card.min.y + 4.0f},
                              {card.max.x - 4.0f, card.min.y + 4.0f + prev_h}};
        fg->AddRect(card.min, card.max, ImGui::GetColorU32(ImVec4(1, 1, 1, 0.18f)), 8.0f);
        FramePattern::draw(fg, pic, clock.seed(), at);

        const std::string label = PlaybackClock::format_time(at);
        const ImVec2      ls    = ImGui::CalcTextSize(label.c_str());
        fg->AddText({card.center().x - ls.x * 0.5f, pic.max.y + 3.0f},
                    ImGui::GetColorU32(ImVec4(1, 1, 1, 0.9f)), label.c_str());
    }

    ImGui::Dummy({0.0f, 4.0f});

    // ── Control row ──────────────────────────────────────────────────────────
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.0f, 5.0f});

    // Captured BEFORE anything is drawn on this row: once an item has been emitted,
    // GetContentRegionAvail() measures from the wrapped cursor on the NEXT line, not
    // from the end of the row, and right-aligning off that number puts the cluster
    // past the window edge.
    const float row_left  = ImGui::GetCursorPosX();
    const float row_width = ImGui::GetContentRegionAvail().x;

    const auto tool_button = [&](const char *glyph, const char *tip, bool accented) -> bool {
        if (accented)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.62f, 0.95f, 0.85f));
        const bool pressed = ImGui::Button(glyph);
        if (accented)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tip);
        return pressed;
    };

    if (tool_button("|<", "Previous clip", false))
        switch_relative = -1;
    ImGui::SameLine();
    if (tool_button("<<", "Back", false)) {
        clock.seek_by(-seek_step_seconds);
        stage.toast("Seek -" + std::to_string(seek_step_seconds) + "s");
    }
    ImGui::SameLine();
    if (tool_button(clock.paused() ? " > " : " || ", clock.paused() ? "Play" : "Pause", false)) {
        clock.toggle_pause();
        stage.toast(clock.paused() ? "Paused" : "Playing");
    }
    ImGui::SameLine();
    if (tool_button(">>", "Forward", false)) {
        clock.seek_by(seek_step_seconds);
        stage.toast("Seek +" + std::to_string(seek_step_seconds) + "s");
    }
    ImGui::SameLine();
    if (tool_button("loop", "Loop this clip", clock.loop())) {
        clock.toggle_loop();
        stage.toast(clock.loop() ? "Loop On" : "Loop Off");
    }
    ImGui::SameLine();
    if (tool_button(">|", "Next clip", false))
        switch_relative = 1;

    // Clock readout follows the knob while scrubbing, not the (lagging) position.
    const double shown_time =
        m_scrubbing ? static_cast<double>(m_scrub_fraction) * clock.duration() : clock.position();
    ImGui::SameLine(0.0f, 14.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(PlaybackClock::format_time(shown_time).c_str());
    ImGui::SameLine(0.0f, 3.0f);
    ImGui::TextDisabled("/");
    ImGui::SameLine(0.0f, 3.0f);
    ImGui::TextDisabled("%s", PlaybackClock::format_time(clock.duration()).c_str());

    ImGui::SameLine(0.0f, 14.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", clock.title().c_str());

    // ── Right cluster: volume + fullscreen ───────────────────────────────────
    // Right-aligned by jumping the cursor to (row end - cluster width), so it stays
    // pinned to the edge as the window resizes instead of drifting with the controls
    // on the left. Falls back to flowing inline when the row is too narrow to split.
    constexpr float k_volume_w = 150.0f;
    const float     fs_w   = ImGui::CalcTextSize("[  ]").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float     cluster = k_volume_w + fs_w + ImGui::GetStyle().ItemSpacing.x;
    const float     cluster_x = row_left + row_width - cluster;

    ImGui::SameLine();
    if (cluster_x > ImGui::GetCursorPosX() + 12.0f)
        ImGui::SetCursorPosX(cluster_x);

    int volume = clock.volume();
    ImGui::SetNextItemWidth(k_volume_w);
    if (ImGui::SliderInt("##vol", &volume, 0, 150, "vol %d%%")) {
        clock.set_volume(volume);
        stage.toast("Volume " + std::to_string(clock.volume()) + "%");
    }

    ImGui::SameLine();
    if (tool_button("[  ]", fullscreen ? "Leave fullscreen" : "Fullscreen", fullscreen)) {
        fullscreen = !fullscreen;
        stage.toast(fullscreen ? "Fullscreen" : "Windowed");
    }

    ImGui::PopStyleVar();
    return switch_relative;
}
