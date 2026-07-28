#include "pch.hpp" // NOLINT

#include "video_stage.hpp"

#include "frame_pattern.hpp"
#include "motion.hpp"
#include "playback_clock.hpp"

void VideoStage::toast(std::string text, double seconds)
{
    m_toast       = std::move(text);
    m_toast_until = ImGui::GetTime() + seconds;
}

void VideoStage::draw(const PanelBounds &stage, PlaybackClock &clock, bool &fullscreen, float dt)
{
    ImDrawList  *dl  = ImGui::GetWindowDrawList();
    const double now = ImGui::GetTime();

    // ── Letterbox ────────────────────────────────────────────────────────────
    const float stage_aspect = (stage.height() > 0.0f) ? stage.width() / stage.height() : 1.0f;
    float       pw           = stage.width();
    float       ph           = stage.height();
    if (aspect > stage_aspect)
        ph = stage.width() / aspect;
    else
        pw = stage.height() * aspect;

    const ImVec2 c = stage.center();
    m_picture      = {{c.x - pw * 0.5f, c.y - ph * 0.5f}, {c.x + pw * 0.5f, c.y + ph * 0.5f}};

    dl->AddRectFilled(stage.min, stage.max, ImGui::GetColorU32(ImVec4(0.02f, 0.02f, 0.03f, 1.0f)));
    FramePattern::draw(dl, m_picture, clock.seed(), clock.position());

    // ── Gestures ─────────────────────────────────────────────────────────────
    // An InvisibleButton over the picture rather than IsMouseHoveringRect, so ImGui
    // owns the hit test and the panels overlapping the stage win the press when they
    // are on top of it.
    ImGui::SetCursorScreenPos(m_picture.min);
    ImGui::InvisibleButton("##stage", {std::max(pw, 1.0f), std::max(ph, 1.0f)},
                           ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered();

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_hold_active  = true;
        m_hold_accel   = false;
        m_suppress_tap = false;
        m_press_time   = now;
    }

    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        fullscreen = !fullscreen;
        toast(fullscreen ? "Fullscreen" : "Windowed");
        m_suppress_tap = true; // the release must not also toggle pause
    }

    if (m_hold_active && !m_hold_accel && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        (now - m_press_time) >= k_hold_threshold) {
        m_hold_accel  = true;
        m_saved_speed = clock.speed();
        clock.set_speed(k_hold_speed);
        std::array<char, 32> buf{};
        std::snprintf(buf.data(), buf.size(), "%.2gx", k_hold_speed);
        toast(buf.data(), 60.0); // held open for as long as the button is down
    }

    // The release is handled even when the cursor has since left the picture.
    if (m_hold_active && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (m_hold_accel) {
            clock.set_speed(m_saved_speed);
            toast("1x");
        } else if (!m_suppress_tap && (now - m_press_time) < k_hold_threshold) {
            clock.toggle_pause();
            toast(clock.paused() ? "Paused" : "Playing");
        }
        m_hold_active  = false;
        m_hold_accel   = false;
        m_suppress_tap = false;
    }

    // ── Paused veil ──────────────────────────────────────────────────────────
    // A dim wash plus a play glyph, both eased in. Pausing is a state, so it gets a
    // persistent indicator rather than a toast that fades out and leaves no trace.
    const ImGuiID stage_id = ImHashStr("concept_stage");
    const float   pause_in = Motion::tween_toggle(stage_id, ImHashStr("pause_veil"),
                                                  clock.paused(), 0.25f, EasePreset::OutCubic,
                                                  dt, /*init_on=*/false);
    if (pause_in > 0.002f) {
        dl->AddRectFilled(m_picture.min, m_picture.max,
                          ImGui::GetColorU32(ImVec4(0, 0, 0, 0.35f * pause_in)));
        const ImVec2 pc = m_picture.center();
        const float  r  = std::min(pw, ph) * 0.075f * (0.85f + 0.15f * pause_in);
        const ImU32  col = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.88f * pause_in));
        dl->AddCircle(pc, r * 1.9f, col, 48, 2.0f * pause_in);
        dl->AddTriangleFilled({pc.x - r * 0.45f, pc.y - r * 0.75f},
                              {pc.x - r * 0.45f, pc.y + r * 0.75f},
                              {pc.x + r * 0.80f, pc.y}, col);
    }

    // ── OSD toast ────────────────────────────────────────────────────────────
    if (!m_toast.empty()) {
        // While a hold is accelerating the toast stays pinned; otherwise it fades over
        // its last 0.35 s.
        const double left  = m_toast_until - now;
        const float  alpha = (left <= 0.0) ? 0.0f
                                           : static_cast<float>(std::min(left / 0.35, 1.0));
        if (alpha <= 0.0f) {
            m_toast.clear();
        } else {
            const ImVec2 ts  = ImGui::CalcTextSize(m_toast.c_str());
            const ImVec2 pad{14.0f, 8.0f};
            const ImVec2 at{m_picture.center().x - ts.x * 0.5f,
                            m_picture.min.y + m_picture.height() * 0.12f};
            dl->AddRectFilled({at.x - pad.x, at.y - pad.y}, {at.x + ts.x + pad.x, at.y + ts.y + pad.y},
                              ImGui::GetColorU32(ImVec4(0.03f, 0.03f, 0.04f, 0.72f * alpha)), 6.0f);
            dl->AddText(at, ImGui::GetColorU32(ImVec4(1, 1, 1, alpha)), m_toast.c_str());
        }
    }

    // A hold that ended must not leave its pinned toast on screen.
    if (!m_hold_accel && m_toast_until > now + 2.0)
        m_toast_until = now + 0.35;
}
