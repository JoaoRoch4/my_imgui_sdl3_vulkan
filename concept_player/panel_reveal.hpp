#pragma once

#include "ease_preset.hpp"
#include "panel_bounds.hpp"
#include "panel_edge.hpp"
#include "pch.hpp"

/// Decides whether an auto-hiding panel is on screen right now, and how far through
/// its slide it is.
///
/// The existing player has exactly one of these, hardcoded, shared by the whole
/// control bar: a 1.5 s idle timer that hard-cuts the controls out of the frame
/// (`if (!show_controls) { ImGui::End(); return; }`). Two things are wrong with that
/// for the sketched design, and this class fixes both:
///
///  1. **No way back.** Once hidden in fullscreen there is no gesture that brings a
///     panel back except moving the mouse, which reveals *everything*. Here each
///     panel owns an edge-proximity hot zone: put the cursor within `edge_margin` of
///     the left edge and only the scene view comes out.
///  2. **It is a cut, not a motion.** `shown()` is a continuous 0..1 driven through
///     Motion (ImAnim's model), so the caller slides and fades rather than blinking.
///
/// The panel's own rectangle counts as part of its hot zone while it is out, so the
/// cursor travelling into the panel keeps it there.
class PanelReveal {
public:
    PanelReveal(PanelEdge edge, float thickness);

    /// Seconds the cursor may sit outside the hot zone before the panel retreats.
    float hide_delay = 1.5f;

    /// Thickness of the invisible band along the panel's edge that triggers a reveal.
    /// Generous on purpose: a thin band is a target the user has to aim for, which is
    /// the opposite of what an auto-hiding panel should feel like.
    float edge_margin = 56.0f;

    /// Seconds the slide takes in each direction.
    float slide_duration = 0.22f;

    /// When set, ANY pointer motion over the stage reveals the panel, not just motion
    /// near its edge. That is the behaviour the sketch asks for on the transport bar,
    /// which should come back the moment the viewer stirs.
    bool reveal_on_any_motion = false;

    /// Advances the reveal by one frame and returns the 0..1 shown factor.
    /// @param owner   stable id for the Motion channel (the panel's ImGui id)
    /// @param stage   the video stage rect this panel is anchored inside
    /// @param pinned  user has pinned the panel open; hot-zone logic is bypassed
    /// @param dt      ImGui::GetIO().DeltaTime
    float update(ImGuiID owner, const PanelBounds &stage, bool pinned, float dt);

    /// Treat the panel as active this instant — restarts the idle countdown without
    /// requiring pointer motion. Used when a keyboard action targets the panel.
    void poke();

    [[nodiscard]] float shown() const { return m_shown; }
    [[nodiscard]] bool  hot() const { return m_hot; }

    /// The band that triggers this panel, for debug overlays.
    [[nodiscard]] PanelBounds hot_zone(const PanelBounds &stage) const;

    /// Where the panel sits for a given shown factor: fully out at 1, fully off its
    /// edge at 0.
    [[nodiscard]] PanelBounds slot(const PanelBounds &stage, float shown_factor) const;

private:
    PanelEdge m_edge;
    float     m_thickness;
    float     m_shown         = 0.0f;
    double    m_last_hot_time = -1000.0;
    bool      m_hot           = false;
};
