#pragma once

/// Which side of the video stage a panel is anchored to, and therefore the direction
/// it slides out of view when it auto-hides.
enum class PanelEdge {
    Left,   ///< scene view — the "+5 sec frame" column
    Right,  ///< folder view — hover-to-preview shelf
    Bottom, ///< transport — seek strip, controls, volume
    Top,    ///< menu bar, when fullscreen leaves it nowhere to be docked
};
