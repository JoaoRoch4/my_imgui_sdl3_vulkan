#pragma once

#include "panel_bounds.hpp"
#include "pch.hpp"

/// Draws a stand-in "video frame" for a (clip, timestamp) pair.
///
/// The design questions this program exists to answer — does the scene strip read at
/// a glance, does the hover preview pull the eye, does the transport stay out of the
/// way — all need pictures that CHANGE over time and DIFFER between clips. They do not
/// need a decoder. So a frame here is generated: a seeded palette, a sun that tracks
/// the timestamp, and three parallax ridges that scroll at their own rates.
///
/// Everything is drawn straight into an ImDrawList with no texture, which is why the
/// same call renders a 96 px scene thumbnail and a full-stage frame — and why panels
/// can be popped into other OS windows without any per-viewport GPU resource to track.
class FramePattern {
public:
    /// Draws the frame for @p seed at @p time_sec, clipped to @p rect.
    static void draw(ImDrawList        *draw_list,
                     const PanelBounds &rect,
                     std::uint32_t      seed,
                     double             time_sec,
                     float              rounding = 0.0f);

    /// The clip's signature colour — used for folder badges and strip accents so a
    /// clip is identifiable before its picture is even legible.
    [[nodiscard]] static ImU32 key_color(std::uint32_t seed, float alpha = 1.0f);

private:
    /// Deterministic 0..1 hash of (seed, salt). Frames must look identical every time
    /// they are drawn, so nothing here may touch a global RNG.
    [[nodiscard]] static float hash01(std::uint32_t seed, std::uint32_t salt);
};
