#pragma once

#include "panel_bounds.hpp"
#include "pch.hpp"

class PlaybackClock;
class VideoStage;

/// The bottom bar, laid out the way the sketch draws it: a full-width seek strip
/// spanning the whole player ABOVE a row of controls, transport on the left, volume on
/// the right.
///
/// That split is the substantive change from the current player, where the seek slider
/// is squeezed inline between the transport buttons and the volume and gets whatever
/// width is left over. Giving the scrub its own full-width row is what makes precise
/// seeking possible on a long clip, and it is why the bar is drawn by hand here rather
/// than with ImGui::SliderFloat — the strip has to be thin at rest, grow under the
/// cursor, and carry a preview, none of which a stock slider does.
class TransportBar {
public:
    /// Seconds the skip buttons and the arrow keys jump.
    int seek_step_seconds = 5;

    /// Resting and hovered thickness of the seek strip, in pixels.
    float bar_thickness_idle  = 4.0f;
    float bar_thickness_hover = 9.0f;

    /// Draws the bar into the current ImGui window. Returns -1 / +1 when the user asked
    /// for the previous or next clip, 0 otherwise.
    int draw(PlaybackClock &clock, VideoStage &stage, bool &fullscreen, float dt);

    /// True while the user is dragging the seek strip — the caller keeps the panel
    /// revealed and suppresses the auto-hide for the duration.
    [[nodiscard]] bool scrubbing() const { return m_scrubbing; }

private:
    bool  m_scrubbing      = false;
    float m_scrub_fraction = 0.0f;
};
