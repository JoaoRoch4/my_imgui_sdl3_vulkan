#pragma once

#include "media_tile.hpp"
#include "panel_bounds.hpp"
#include "pch.hpp"

/// The sketch's right-hand folder view: the clips sitting next to the one playing,
/// each a thumbnail with a play badge, each previewable by hovering it.
///
/// The preview plays **inline** — the hovered tile's own thumbnail starts running,
/// its play badge fades out, and a progress line tracks the position along its bottom
/// edge. No floating card, no popup. Two reasons that is the better behaviour here:
/// the preview appears exactly where the eye already is, and it cannot cover the video
/// it is meant to be compared against.
///
/// The dwell before a hover counts is the same 300 ms the real VideoHoverPreview uses,
/// and for the same reason: without it, dragging the cursor down a list spawns and
/// kills a preview for every row it crosses. Here that only wastes a few draw calls;
/// in the real player each one is an mpv instance and a network fetch.
class FolderShelf {
public:
    /// Seconds the cursor must rest on a tile before its preview starts.
    float hover_dwell = 0.30f;

    /// Seconds the tile takes to cross-fade from poster frame to running preview.
    float preview_fade = 0.18f;

    void set_items(std::vector<MediaTile> items);

    /// Draws the shelf into the current ImGui window. Returns the clicked index, or -1.
    /// @param current_index the clip currently playing, drawn as selected
    int draw(int current_index, float dt);

    /// Index whose dwell has elapsed and which is therefore playing inline, or -1.
    [[nodiscard]] int previewing_index() const { return m_previewing; }

    [[nodiscard]] const std::vector<MediaTile> &items() const { return m_items; }

private:
    std::vector<MediaTile> m_items;
    int                    m_hovered      = -1;
    int                    m_previewing   = -1;
    double                 m_hover_start  = 0.0;
    double                 m_preview_time = 0.0;
};
