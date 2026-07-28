#pragma once

#include "panel_bounds.hpp"
#include "pch.hpp"
#include "scene_frame.hpp"

class PlaybackClock;

/// The sketch's left-hand scene view: one thumbnail every N seconds down the clip,
/// click to jump there.
///
/// In the real player each tile is a decoded frame. The existing VideoSeekPreview
/// already renders one arbitrary timestamp on a private mpv instance; a strip is that
/// mechanism run over a list of timestamps with the results cached per tile. Which is
/// why this widget is written against a vector<SceneFrame> and asks nothing of where
/// the pictures come from — swapping generated frames for decoded ones touches the
/// draw call and nothing else.
class SceneStrip {
public:
    /// Seconds between tiles — the "+5 sec" of the sketch, adjustable in the View menu.
    float step_seconds = 5.0f;

    /// Rebuilds the timestamp list if the clip or the step changed. Cheap to call
    /// every frame; it compares against what it last built from.
    void sync(const PlaybackClock &clock);

    /// Draws the strip into the current ImGui window. Returns the clicked timestamp, or a
    /// negative value if they did not click one.
    double draw(const PlaybackClock &clock, float dt);

    [[nodiscard]] int frame_count() const { return static_cast<int>(m_frames.size()); }

private:
    std::vector<SceneFrame> m_frames;
    std::uint32_t           m_built_seed     = 0;
    double                  m_built_duration = -1.0;
    float                   m_built_step     = -1.0f;
    bool                    m_follow_playhead = true;
};
