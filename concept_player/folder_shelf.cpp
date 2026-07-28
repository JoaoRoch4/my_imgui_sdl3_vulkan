#include "pch.hpp" // NOLINT

#include "folder_shelf.hpp"

#include "frame_pattern.hpp"
#include "motion.hpp"
#include "playback_clock.hpp"

void FolderShelf::set_items(std::vector<MediaTile> items)
{
    m_items      = std::move(items);
    m_hovered    = -1;
    m_previewing = -1;
}

int FolderShelf::draw(int current_index, float dt)
{
    int          clicked = -1;
    const double now     = ImGui::GetTime();

    if (m_items.empty()) {
        ImGui::TextDisabled("Folder is empty");
        m_previewing = -1;
        return clicked;
    }

    if (!ImGui::BeginChild("##folder_scroll", {0.0f, 0.0f}, ImGuiChildFlags_None,
                           ImGuiWindowFlags_NoBackground)) {
        ImGui::EndChild();
        return clicked;
    }

    const float tile_w = std::max(ImGui::GetContentRegionAvail().x, 32.0f);
    const float tile_h = tile_w * 9.0f / 16.0f;
    ImDrawList *dl     = ImGui::GetWindowDrawList();

    int hovered_this_frame = -1;

    for (int i = 0; i < static_cast<int>(m_items.size()); ++i) {
        const MediaTile &tile       = m_items[static_cast<std::size_t>(i)];
        const bool       is_current = (i == current_index);

        ImGui::PushID(i);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float  row_h  = tile_h + ImGui::GetTextLineHeight() * 2.0f + 8.0f;
        ImGui::InvisibleButton("##tile", {tile_w, row_h});
        const bool hovered = ImGui::IsItemHovered();
        if (hovered)
            hovered_this_frame = i;
        if (ImGui::IsItemClicked())
            clicked = i;

        const ImGuiID tile_id = ImGui::GetItemID();
        const float   grow =
            Motion::tween_float(tile_id, ImHashStr("grow"), hovered ? 1.0f : 0.0f, 0.16f,
                                EasePreset::OutCubic, dt, 0.0f);

        // Hovered tiles slide slightly out of the shelf toward the video, which reads
        // as "this one is being pulled forward" rather than "this one got bigger".
        const float lift  = -8.0f * grow;
        const float inset = 5.0f * (1.0f - grow);

        const PanelBounds pic{{origin.x + inset + lift, origin.y + inset * 0.5f},
                              {origin.x + tile_w - inset + lift, origin.y + tile_h - inset * 0.5f}};

        // ── Inline preview ───────────────────────────────────────────────────
        // At rest a tile shows its poster frame — a fixed point 1/3 in, so a clip is
        // recognisable and does not flicker as the main clock advances. Once the
        // hover dwell elapses the SAME tile starts playing in place: the timestamp
        // handed to FramePattern becomes the running preview clock instead.
        const bool  is_previewing = (i == m_previewing);
        const float preview_in =
            Motion::tween_toggle(tile_id, ImHashStr("preview"), is_previewing, preview_fade,
                                 EasePreset::OutCubic, dt, /*init_on=*/false);

        FramePattern::draw(dl, pic, tile.seed,
                           is_previewing ? m_preview_time : tile.duration * 0.33);

        const ImU32 ring = is_current ? FramePattern::key_color(tile.seed, 0.95f)
                                      : ImGui::GetColorU32(ImVec4(1, 1, 1, 0.10f + 0.5f * grow));
        dl->AddRect(pic.min, pic.max, ring, 5.0f, 0, is_current ? 2.5f : 1.0f);

        // ── Play badge ───────────────────────────────────────────────────────
        // The sketch draws one on every folder thumbnail; it is what marks the shelf
        // as a list of playable things rather than a list of images. It fades out as
        // the inline preview takes over — the tile is now showing motion, so the badge
        // has nothing left to promise.
        const float badge_alpha = 1.0f - preview_in;
        if (tile.is_video && badge_alpha > 0.002f) {
            const ImVec2 pc = pic.center();
            const float  r  = std::min(pic.width(), pic.height()) * (0.13f + 0.03f * grow);
            dl->AddCircleFilled(
                pc, r * 1.7f,
                ImGui::GetColorU32(ImVec4(0, 0, 0, (0.35f + 0.2f * grow) * badge_alpha)));
            const ImU32 glyph =
                ImGui::GetColorU32(ImVec4(1, 1, 1, (0.80f + 0.20f * grow) * badge_alpha));
            dl->AddTriangleFilled({pc.x - r * 0.45f, pc.y - r * 0.8f},
                                  {pc.x - r * 0.45f, pc.y + r * 0.8f},
                                  {pc.x + r * 0.85f, pc.y}, glyph);
        }

        // ── Inline progress line ─────────────────────────────────────────────
        // Sits along the tile's bottom edge while it previews, so the tile reads as
        // playing rather than as an animated thumbnail, and shows where in the clip
        // the preview has reached.
        if (preview_in > 0.002f && tile.duration > 0.0) {
            const float frac = static_cast<float>(
                std::clamp(m_preview_time / tile.duration, 0.0, 1.0));
            const float line_y = pic.max.y - 3.0f;
            dl->AddLine({pic.min.x, line_y}, {pic.max.x, line_y},
                        ImGui::GetColorU32(ImVec4(1, 1, 1, 0.22f * preview_in)), 2.5f);
            dl->AddLine({pic.min.x, line_y},
                        {pic.min.x + pic.width() * frac, line_y},
                        ImGui::GetColorU32(ImVec4(0.91f, 0.24f, 0.28f, 0.95f * preview_in)), 2.5f);
        }

        // Duration chip, bottom-right of the thumbnail.
        const std::string dur = PlaybackClock::format_time(tile.duration);
        const ImVec2      ds  = ImGui::CalcTextSize(dur.c_str());
        dl->AddRectFilled({pic.max.x - ds.x - 10.0f, pic.max.y - ds.y - 8.0f},
                          {pic.max.x - 4.0f, pic.max.y - 2.0f},
                          ImGui::GetColorU32(ImVec4(0, 0, 0, 0.62f)), 3.0f);
        dl->AddText({pic.max.x - ds.x - 7.0f, pic.max.y - ds.y - 5.0f},
                    ImGui::GetColorU32(ImVec4(1, 1, 1, 0.85f)), dur.c_str());

        // The label does NOT take the hover lift: the thumbnail sliding toward the
        // video is the intended motion, but sliding the text with it pushes its first
        // character out of the panel's clip rect and reads as a rendering glitch.
        dl->AddText({origin.x + 2.0f, origin.y + tile_h + 3.0f},
                    ImGui::GetColorU32(is_current ? ImVec4(1, 1, 1, 1) : ImVec4(1, 1, 1, 0.62f)),
                    tile.name.c_str());

        ImGui::PopID();
    }

    ImGui::EndChild();

    // ── Dwell ────────────────────────────────────────────────────────────────
    // Moving to a different tile restarts the countdown and drops the current preview,
    // so sweeping down the shelf never leaves a stale one running.
    if (hovered_this_frame != m_hovered) {
        m_hovered     = hovered_this_frame;
        m_hover_start = now;
        m_previewing  = -1;
    }
    if (m_hovered >= 0 && (now - m_hover_start) >= static_cast<double>(hover_dwell)) {
        // Starting a fresh preview seeks a third of the way in: the opening seconds of
        // a clip are usually the least representative part of it.
        if (m_previewing != m_hovered) {
            m_previewing   = m_hovered;
            m_preview_time = m_items[static_cast<std::size_t>(m_previewing)].duration * 0.33;
        }
    }

    if (m_previewing >= 0) {
        const double dur = m_items[static_cast<std::size_t>(m_previewing)].duration;
        m_preview_time += static_cast<double>(dt);
        if (dur > 0.0 && m_preview_time > dur)
            m_preview_time = std::fmod(m_preview_time, dur);
    }

    return clicked;
}
