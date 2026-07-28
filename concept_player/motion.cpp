#include "pch.hpp" // NOLINT

#include "motion.hpp"

#include "motion_channel.hpp"

#if CONCEPT_HAS_IMANIM
// Compiled only when external/ImAnim is vendored (CMake sets the define). The calls
// below follow the API documented in .claude/skills/imanim/SKILL.md; if the real
// header's signatures have drifted from that doc, THIS FILE is the only place that
// needs adjusting — no call site touches iam_* directly.
    #include "im_anim.h"
#endif

// Channel table for the fallback path. A file-scope static with a distinctive name
// rather than an anonymous namespace: this target is a unity build, so merged TUs
// would collide on a generic name.
static std::unordered_map<ImU64, MotionChannel> s_motion_channels;
static int                                      s_motion_frame = 0;

/// (owner, channel) packed into the table key — ImAnim's keying model.
static ImU64 motion_key(ImGuiID owner, ImGuiID channel)
{
    return (static_cast<ImU64>(owner) << 32) | static_cast<ImU64>(channel);
}

float Motion::apply_ease(EasePreset ease, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    switch (ease) {
        case EasePreset::Linear:
            return t;
        case EasePreset::OutQuad: {
            const float inv = 1.0f - t;
            return 1.0f - inv * inv;
        }
        case EasePreset::OutCubic: {
            const float inv = 1.0f - t;
            return 1.0f - inv * inv * inv;
        }
        case EasePreset::OutBack: {
            // Overshoot constants are the canonical CSS/Penner "back" values.
            constexpr float c1  = 1.70158f;
            constexpr float c3  = c1 + 1.0f;
            const float     inv = t - 1.0f;
            return 1.0f + c3 * inv * inv * inv + c1 * inv * inv;
        }
        case EasePreset::OutElastic: {
            if (t <= 0.0f || t >= 1.0f)
                return t;
            constexpr float period = 0.3f;
            constexpr float two_pi = 6.28318530718f;
            return std::pow(2.0f, -10.0f * t) *
                       std::sin((t - period * 0.25f) * two_pi / period) +
                   1.0f;
        }
    }
    return t;
}

float Motion::tween_float(ImGuiID    owner,
                          ImGuiID    channel,
                          float      target,
                          float      duration,
                          EasePreset ease,
                          float      dt,
                          float      init)
{
#if CONCEPT_HAS_IMANIM
    iam_easing easing{};
    switch (ease) {
        case EasePreset::Linear:     easing = iam_ease_preset(iam_ease_linear);     break;
        case EasePreset::OutQuad:    easing = iam_ease_preset(iam_ease_out_quad);   break;
        case EasePreset::OutCubic:   easing = iam_ease_preset(iam_ease_out_cubic);  break;
        case EasePreset::OutBack:    easing = iam_ease_back(1.4f);                  break;
        case EasePreset::OutElastic: easing = iam_ease_elastic(1.0f, 0.3f);         break;
    }
    return iam_tween_float(owner, channel, target, duration, easing,
                           iam_policy_crossfade, dt, init);
#else
    MotionChannel &ch = [&]() -> MotionChannel & {
        const ImU64 key = motion_key(owner, channel);
        const auto  it  = s_motion_channels.find(key);
        if (it != s_motion_channels.end())
            return it->second;
        // Born at `init` and already settled there, so a widget appearing at its
        // resting value does not animate in from 0 on its first frame.
        MotionChannel fresh;
        fresh.from = fresh.to = fresh.current = init;
        fresh.elapsed = fresh.duration = 0.0f;
        return s_motion_channels.emplace(key, fresh).first->second;
    }();

    ch.used_frame = s_motion_frame;

    // A new target restarts the leg FROM THE CURRENT VALUE (crossfade policy), which
    // is what keeps a mid-flight reversal smooth instead of snapping back to `from`.
    if (std::fabs(target - ch.to) > 1e-6f) {
        ch.from     = ch.current;
        ch.to       = target;
        ch.elapsed  = 0.0f;
        ch.duration = std::max(duration, 1e-4f);
    }

    if (ch.duration <= 0.0f) {
        ch.current = ch.to;
        return ch.current;
    }

    ch.elapsed += std::max(dt, 0.0f);
    const float t = std::clamp(ch.elapsed / ch.duration, 0.0f, 1.0f);
    ch.current    = ch.from + (ch.to - ch.from) * apply_ease(ease, t);
    if (t >= 1.0f)
        ch.current = ch.to;
    return ch.current;
#endif
}

float Motion::tween_toggle(ImGuiID    owner,
                           ImGuiID    channel,
                           bool       on,
                           float      duration,
                           EasePreset ease,
                           float      dt,
                           bool       init_on)
{
    return tween_float(owner, channel, on ? 1.0f : 0.0f, duration, ease, dt,
                       init_on ? 1.0f : 0.0f);
}

ImVec4 Motion::tween_color(ImGuiID       owner,
                           ImGuiID       channel,
                           const ImVec4 &target,
                           float         duration,
                           EasePreset    ease,
                           float         dt,
                           const ImVec4 &init)
{
#if CONCEPT_HAS_IMANIM
    iam_easing easing = iam_ease_preset(iam_ease_out_quad);
    if (ease == EasePreset::OutCubic)
        easing = iam_ease_preset(iam_ease_out_cubic);
    return iam_tween_color(owner, channel, target, duration, easing,
                           iam_policy_crossfade, iam_col_oklab, dt, init);
#else
    // Blend in linear space, not raw sRGB: a straight lerp between two saturated sRGB
    // colours dips through a muddy midpoint. Approximating gamma 2.2 with a square is
    // close enough at UI sizes and avoids a pow() per component per frame.
    const auto to_linear   = [](float c) { return c * c; };
    const auto to_srgb     = [](float c) { return std::sqrt(std::max(c, 0.0f)); };
    const ImGuiID sub[4]   = {channel ^ 0x1u, channel ^ 0x2u, channel ^ 0x3u, channel ^ 0x4u};
    const float   tgt[4]   = {to_linear(target.x), to_linear(target.y),
                              to_linear(target.z), target.w};
    const float   start[4] = {to_linear(init.x), to_linear(init.y),
                              to_linear(init.z), init.w};

    ImVec4 out;
    out.x = to_srgb(tween_float(owner, sub[0], tgt[0], duration, ease, dt, start[0]));
    out.y = to_srgb(tween_float(owner, sub[1], tgt[1], duration, ease, dt, start[1]));
    out.z = to_srgb(tween_float(owner, sub[2], tgt[2], duration, ease, dt, start[2]));
    out.w = tween_float(owner, sub[3], tgt[3], duration, ease, dt, start[3]);
    return out;
#endif
}

void Motion::begin_frame()
{
#if CONCEPT_HAS_IMANIM
    iam_update_begin_frame();
#else
    ++s_motion_frame;
#endif
}

void Motion::gc(int max_idle_frames)
{
#if CONCEPT_HAS_IMANIM
    iam_gc(max_idle_frames);
#else
    if (max_idle_frames <= 0)
        return;
    std::erase_if(s_motion_channels, [max_idle_frames](const auto &entry) {
        return s_motion_frame - entry.second.used_frame > max_idle_frames;
    });
#endif
}

bool Motion::using_imanim()
{
#if CONCEPT_HAS_IMANIM
    return true;
#else
    return false;
#endif
}

int Motion::live_channel_count()
{
#if CONCEPT_HAS_IMANIM
    return -1; // ImAnim owns the table; it does not publish a count.
#else
    return static_cast<int>(s_motion_channels.size());
#endif
}
