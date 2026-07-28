#include "pch.hpp" // NOLINT

#include "frame_pattern.hpp"

float FramePattern::hash01(std::uint32_t seed, std::uint32_t salt)
{
    // Integer avalanche (Wang-style), then take the top bits. Deterministic across
    // runs and platforms — a frame must look the same every time it is drawn.
    std::uint32_t h = seed * 0x9E3779B9u + salt * 0x85EBCA6Bu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

ImU32 FramePattern::key_color(std::uint32_t seed, float alpha)
{
    const float hue = hash01(seed, 1u);
    float       r   = 0.0f;
    float       g   = 0.0f;
    float       b   = 0.0f;
    ImGui::ColorConvertHSVtoRGB(hue, 0.62f, 0.92f, r, g, b);
    return ImGui::GetColorU32(ImVec4(r, g, b, alpha));
}

void FramePattern::draw(ImDrawList        *draw_list,
                        const PanelBounds &rect,
                        std::uint32_t      seed,
                        double             time_sec,
                        float              rounding)
{
    const float w = rect.width();
    const float h = rect.height();
    if (!draw_list || w < 2.0f || h < 2.0f)
        return;

    const float t = static_cast<float>(time_sec);

    // ── Palette ──────────────────────────────────────────────────────────────
    // The base hue belongs to the clip; a slow drift over the timeline makes the
    // picture visibly progress, which is the whole point of the scene strip.
    const float base_hue = hash01(seed, 1u);
    const float hue      = std::fmod(base_hue + t * 0.006f, 1.0f);

    const auto hsv = [](float hh, float ss, float vv, float aa) {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        ImGui::ColorConvertHSVtoRGB(std::fmod(hh + 1.0f, 1.0f), ss, vv, r, g, b);
        return ImGui::GetColorU32(ImVec4(r, g, b, aa));
    };

    const ImU32 sky_top    = hsv(hue, 0.55f, 0.30f, 1.0f);
    const ImU32 sky_bottom = hsv(hue + 0.08f, 0.70f, 0.72f, 1.0f);

    // Clip so the ridges and the sun cannot bleed past the frame into the panel.
    draw_list->PushClipRect(rect.min, rect.max, true);
    draw_list->AddRectFilledMultiColor(rect.min, rect.max, sky_top, sky_top, sky_bottom,
                                       sky_bottom);

    // ── Sun ──────────────────────────────────────────────────────────────────
    // Tracks the timestamp: crosses the frame once every 40 s and dips with a slow
    // sine, so two thumbnails a few seconds apart are visibly different.
    const float sun_phase = std::fmod(t / 40.0f + hash01(seed, 2u), 1.0f);
    const ImVec2 sun{rect.min.x + w * sun_phase,
                     rect.min.y + h * (0.16f + 0.10f * std::sin(t * 0.35f))};
    const float sun_r = std::max(h * 0.07f, 2.0f);
    draw_list->AddCircleFilled(sun, sun_r * 2.6f, hsv(hue + 0.10f, 0.55f, 1.0f, 0.16f));
    draw_list->AddCircleFilled(sun, sun_r, hsv(hue + 0.10f, 0.28f, 1.0f, 0.95f));

    // ── Parallax ridges ──────────────────────────────────────────────────────
    // Three bands, each darker, lower and faster than the one behind it. Built as a
    // path along the ridge line then closed down the sides — concave, so it needs
    // PathFillConcave rather than the convex fast path.
    constexpr int k_bands = 3;
    // Segment count scales with width but stays bounded: a 96 px strip thumbnail does
    // not need the 200 segments a fullscreen stage would use.
    const int segments = std::clamp(static_cast<int>(w / 6.0f), 10, 140);

    for (int band = 0; band < k_bands; ++band) {
        const float depth  = static_cast<float>(band + 1) / static_cast<float>(k_bands);
        const float scroll = t * (0.05f + 0.11f * depth) + hash01(seed, 10u + static_cast<std::uint32_t>(band)) * 10.0f;
        const float base_y = rect.min.y + h * (0.52f + 0.16f * depth);
        const float amp    = h * (0.10f - 0.02f * depth);
        const float freq   = 1.6f + 1.3f * hash01(seed, 20u + static_cast<std::uint32_t>(band));

        for (int i = 0; i <= segments; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(segments);
            const float x = rect.min.x + u * w;
            // Two summed sines: one long swell, one shorter ripple — enough to read as
            // terrain instead of a wave.
            const float y = base_y - amp * (std::sin((u * freq + scroll) * 6.2831853f) * 0.6f +
                                            std::sin((u * freq * 2.3f - scroll * 1.7f) * 6.2831853f) * 0.4f);
            draw_list->PathLineTo({x, y});
        }
        draw_list->PathLineTo({rect.max.x, rect.max.y});
        draw_list->PathLineTo({rect.min.x, rect.max.y});
        draw_list->PathFillConcave(hsv(hue - 0.04f * depth, 0.60f, 0.30f - 0.09f * depth, 1.0f));
    }

    // ── Vignette + film grain-ish top/bottom falloff ─────────────────────────
    const ImU32 clear = ImGui::GetColorU32(ImVec4(0, 0, 0, 0));
    const ImU32 dark  = ImGui::GetColorU32(ImVec4(0, 0, 0, 0.35f));
    draw_list->AddRectFilledMultiColor(rect.min, {rect.max.x, rect.min.y + h * 0.28f}, dark,
                                       dark, clear, clear);
    draw_list->AddRectFilledMultiColor({rect.min.x, rect.max.y - h * 0.30f}, rect.max, clear,
                                       clear, dark, dark);

    draw_list->PopClipRect();

    if (rounding > 0.0f) {
        // Corner mask: the clip rect above is rectangular, so round the frame by
        // drawing the panel-coloured corners back over it.
        draw_list->AddRect(rect.min, rect.max, ImGui::GetColorU32(ImVec4(0, 0, 0, 0.5f)),
                           rounding, 0, 1.0f);
    }
}
