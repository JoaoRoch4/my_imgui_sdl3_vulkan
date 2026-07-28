#pragma once

#include "panel_bounds.hpp"
#include "pch.hpp"

class PlaybackClock;

/// The centre canvas: the picture itself, plus the gestures that live on it.
///
/// The gesture set is carried over verbatim from the current player, because it is the
/// part of that UI worth keeping — tap to pause, press-and-hold to run fast while held,
/// double-click for fullscreen. Reproducing it here keeps the concept honest: the new
/// chrome has to coexist with these, and the auto-hiding panels must not steal the
/// press that starts a hold.
class VideoStage {
public:
    /// Playback speed while the left button is held past the hold threshold.
    static constexpr double k_hold_speed = 2.0;

    /// How long a press must last before it counts as a hold rather than a tap.
    static constexpr double k_hold_threshold = 0.18;

    /// Draws the letterboxed picture into @p stage and services the gestures.
    /// @param fullscreen toggled in place by a double-click
    void draw(const PanelBounds &stage, PlaybackClock &clock, bool &fullscreen, float dt);

    /// Shows a transient centred message (the OSD), replacing any current one.
    void toast(std::string text, double seconds = 1.1);

    /// The rect the picture actually occupies after letterboxing — panels use it to
    /// place themselves against the picture rather than the window.
    [[nodiscard]] const PanelBounds &picture() const { return m_picture; }

    /// 16:9 unless something changes it; the letterbox maths reads from here.
    float aspect = 16.0f / 9.0f;

private:
    PanelBounds m_picture{};

    // Press FSM — mirrors the HoldSpeed struct in the current video_ui_window.cpp.
    bool   m_hold_active   = false;
    bool   m_hold_accel    = false;
    bool   m_suppress_tap  = false;
    double m_press_time    = 0.0;
    double m_saved_speed   = 1.0;

    std::string m_toast;
    double      m_toast_until = 0.0;
};
