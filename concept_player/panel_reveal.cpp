#include "pch.hpp" // NOLINT

#include "panel_reveal.hpp"

#include "motion.hpp"

PanelReveal::PanelReveal(PanelEdge edge, float thickness)
    : m_edge(edge)
    , m_thickness(thickness)
{
}

PanelBounds PanelReveal::hot_zone(const PanelBounds &stage) const
{
    // While the panel is out, its own footprint is part of the zone — otherwise the
    // cursor would "fall through" the panel it is reaching for and start the hide
    // countdown while hovering it.
    const float reach = std::max(edge_margin, m_thickness * m_shown);
    switch (m_edge) {
        case PanelEdge::Left:
            return {stage.min, {stage.min.x + reach, stage.max.y}};
        case PanelEdge::Right:
            return {{stage.max.x - reach, stage.min.y}, stage.max};
        case PanelEdge::Bottom:
            return {{stage.min.x, stage.max.y - reach}, stage.max};
        case PanelEdge::Top:
            return {stage.min, {stage.max.x, stage.min.y + reach}};
    }
    return stage;
}

PanelBounds PanelReveal::slot(const PanelBounds &stage, float shown_factor) const
{
    // Slide by the hidden fraction so the panel emerges from beyond its edge rather
    // than growing in place.
    const float hidden = (1.0f - shown_factor) * m_thickness;
    switch (m_edge) {
        case PanelEdge::Left:
            return {{stage.min.x - hidden, stage.min.y},
                    {stage.min.x - hidden + m_thickness, stage.max.y}};
        case PanelEdge::Right:
            return {{stage.max.x + hidden - m_thickness, stage.min.y},
                    {stage.max.x + hidden, stage.max.y}};
        case PanelEdge::Bottom:
            return {{stage.min.x, stage.max.y + hidden - m_thickness},
                    {stage.max.x, stage.max.y + hidden}};
        case PanelEdge::Top:
            return {{stage.min.x, stage.min.y - hidden},
                    {stage.max.x, stage.min.y - hidden + m_thickness}};
    }
    return stage;
}

void PanelReveal::poke()
{
    m_last_hot_time = ImGui::GetTime();
}

float PanelReveal::update(ImGuiID owner, const PanelBounds &stage, bool pinned, float dt)
{
    const ImGuiIO &io  = ImGui::GetIO();
    const double   now = ImGui::GetTime();

    const bool pointer_valid = io.MousePos.x > -FLT_MAX * 0.5f;
    const bool moved         = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
    const bool clicked       = ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                         ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                         ImGui::IsMouseClicked(ImGuiMouseButton_Middle);

    m_hot = pointer_valid && hot_zone(stage).contains(io.MousePos);

    // A transport bar set to reveal_on_any_motion comes back on any stir of the
    // pointer over the stage; the side panels only wake for their own edge.
    const bool woken_by_motion =
        reveal_on_any_motion && pointer_valid && (moved || clicked) && stage.contains(io.MousePos);

    if (m_hot || woken_by_motion)
        m_last_hot_time = now;

    // Dragging a slider that lives in this panel must hold it open even when the
    // cursor wanders off the hot zone mid-drag.
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && (now - m_last_hot_time) < hide_delay)
        m_last_hot_time = now;

    const bool want_shown = pinned || m_hot || (now - m_last_hot_time) < hide_delay;

    // OutCubic: quick off the mark, settles softly — the slide reads as the panel
    // arriving rather than being thrown.
    m_shown = Motion::tween_toggle(owner,
                                   ImHashStr("panel_shown"),
                                   want_shown,
                                   slide_duration,
                                   EasePreset::OutCubic,
                                   dt,
                                   /*init_on=*/false);
    return m_shown;
}
