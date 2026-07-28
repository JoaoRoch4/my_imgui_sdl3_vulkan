#pragma once

#include "pch.hpp"

/// An axis-aligned screen-space rectangle.
///
/// Deliberately a named type rather than a pair of loose ImVec2s or ImGui's internal
/// ImRect: layout maths is passed between the stage, the panels and the widgets on
/// every frame, and a named type keeps those call sites checkable (and keeps this
/// target off imgui_internal.h).
struct PanelBounds {
    ImVec2 min{0.0f, 0.0f};
    ImVec2 max{0.0f, 0.0f};

    [[nodiscard]] float  width() const { return max.x - min.x; }
    [[nodiscard]] float  height() const { return max.y - min.y; }
    [[nodiscard]] ImVec2 size() const { return {width(), height()}; }
    [[nodiscard]] ImVec2 center() const
    {
        return {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f};
    }

    [[nodiscard]] bool contains(const ImVec2 &p) const
    {
        return p.x >= min.x && p.x < max.x && p.y >= min.y && p.y < max.y;
    }

    /// Shrunk by @p x horizontally and @p y vertically on every side. Never inverts:
    /// over-insetting a small rect collapses it to its centre instead of flipping it.
    [[nodiscard]] PanelBounds inset(float x, float y) const
    {
        const ImVec2 c = center();
        return {{std::min(min.x + x, c.x), std::min(min.y + y, c.y)},
                {std::max(max.x - x, c.x), std::max(max.y - y, c.y)}};
    }
};
