#pragma once

#include "panel_bounds.hpp"
#include "panel_edge.hpp"
#include "panel_placement.hpp"
#include "panel_reveal.hpp"
#include "pch.hpp"

/// A body of player content that can live in one of two places: riding over the video
/// as an auto-hiding overlay, or torn off into its own OS window.
///
/// This is the concept's answer to "where do the scene view and the folder view go?".
/// Rather than making them fixed chrome — which is what forces the current player to
/// choose between covering the video and not having them at all — a panel is a thing
/// with a *placement*. Overlay placement gives the sketch's behaviour: hidden by
/// default, revealed by reaching for its edge, slid and faded by PanelReveal. Popped-out
/// placement makes it a persistent window you can park on another monitor, at which
/// point auto-hide is meaningless and is switched off.
///
/// Usage mirrors ImGui's own Begin/End contract:
///
///     if (panel.begin(stage, dt)) {
///         ... draw content; the panel IS the current ImGui window ...
///         panel.end();
///     }
///
/// end() must be called if and only if begin() returned true.
class ConceptPanel {
public:
    ConceptPanel(std::string id, std::string title, PanelEdge edge, float thickness);

    /// Lays the panel out and opens its window. Returns false when there is nothing
    /// to draw (fully hidden overlay, or a collapsed popped-out window), in which case
    /// end() must NOT be called.
    bool begin(const PanelBounds &stage, float dt);
    void end();

    /// Draws the panel's own header row — title, pin toggle, pop-out/dock toggle.
    /// Separate from begin() so content can decide where the header sits (or skip it).
    void draw_header();

    [[nodiscard]] float              shown() const { return m_reveal.shown(); }
    [[nodiscard]] PanelPlacement     placement() const { return m_placement; }
    [[nodiscard]] bool               pinned() const { return m_pinned; }
    [[nodiscard]] bool               enabled() const { return m_enabled; }
    [[nodiscard]] const std::string &title() const { return m_title; }
    [[nodiscard]] ImGuiID            owner_id() const { return m_owner_id; }
    [[nodiscard]] float              thickness() const { return m_thickness; }

    /// Pixels this panel currently takes out of the stage along its edge: its full
    /// thickness when out, zero when retracted, and zero when popped out into its own
    /// window. Callers use it to keep other panels clear of it.
    [[nodiscard]] float occupied() const
    {
        if (!m_enabled || m_placement != PanelPlacement::Overlay)
            return 0.0f;
        return m_thickness * m_reveal.shown();
    }

    void set_placement(PanelPlacement placement);
    void set_pinned(bool pinned) { m_pinned = pinned; }
    void set_enabled(bool enabled) { m_enabled = enabled; }
    void toggle_placement();

    /// Tunables live on the reveal; exposed so the app's View menu can drive them.
    PanelReveal &reveal() { return m_reveal; }

    /// Opacity of the panel's own background at full reveal. Multiplied by shown(), so
    /// a panel fades as it slides rather than sliding at full strength. The menu bar
    /// runs much lower than the default — it sits directly over the picture and only
    /// needs to be legible, not to block what is behind it.
    float background_alpha = 0.90f;

    /// Window flags OR'd into the panel's own set. The menu bar panel needs
    /// ImGuiWindowFlags_MenuBar; nothing else uses this.
    ImGuiWindowFlags extra_window_flags = 0;

    /// Whether this panel may be torn off at all. False for the menu bar: a menu bar
    /// parked in a window of its own is a thing you can build and nobody wants.
    bool allow_popout = true;

private:
    std::string    m_id;
    std::string    m_title;
    PanelEdge      m_edge;
    float          m_thickness;
    PanelPlacement m_placement = PanelPlacement::Overlay;
    bool           m_pinned    = false;
    bool           m_enabled   = true;
    bool           m_open      = true;

    PanelReveal m_reveal;
    ImGuiID     m_owner_id = 0;

    /// Frame the placement last flipped. The frame after a pop-out we force the window
    /// position outside the main viewport — that is what makes ImGui's viewport system
    /// spawn a real platform window for it — and then never touch the position again,
    /// so the user can move it wherever they like.
    int  m_placement_changed_frame = -1;
    bool m_style_pushed            = false;
};
