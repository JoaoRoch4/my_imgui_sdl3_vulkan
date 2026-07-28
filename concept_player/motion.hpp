#pragma once

#include "ease_preset.hpp"
#include "pch.hpp"

/// The concept's animation seam — and a drop-in adapter for ImAnim.
///
/// The project's `imanim` skill documents external/ImAnim (im_anim.h + im_anim.cpp) as
/// how UI motion is done here, with the call shape
///
///     iam_tween_float(owner, channel, target, duration, easing, policy, dt, init)
///
/// but that library is NOT vendored in this checkout — only .claude/skills/imanim is,
/// on every branch. So Motion reproduces ImAnim's contract exactly:
///
///   * call it UNCONDITIONALLY every frame with a TARGET, not with "start animation"
///   * state lives in a table keyed by (owner ImGuiID, channel ImGuiID)
///   * a target that changes mid-flight is redirected smoothly (crossfade policy)
///   * `init` is the value the channel is born with — set it to the resting target so
///     nothing visibly animates from 0 on the first frame a widget appears
///
/// concept_player/CMakeLists.txt defines CONCEPT_HAS_IMANIM when external/ImAnim
/// appears, and motion.cpp then forwards every call below to the real iam_tween_*.
/// No call site changes. Until then the built-in table below does the same job.
///
/// @note Like ImAnim, this drives VALUES only — callers still do the drawing.
class Motion {
public:
    /// Eases toward @p target and returns this frame's value.
    /// @param owner    stable per-widget id (ImGui::GetID / GetItemID), distinct per widget
    /// @param channel  which property of that widget, e.g. ImHashStr("slide")
    /// @param duration seconds to reach a newly-set target
    /// @param dt       ImGui::GetIO().DeltaTime — do not invent a clock
    /// @param init     value the channel is born with
    static float tween_float(ImGuiID    owner,
                             ImGuiID    channel,
                             float      target,
                             float      duration,
                             EasePreset ease,
                             float      dt,
                             float      init);

    /// Convenience wrapper: eases 0 -> 1 while @p on, 1 -> 0 while not.
    /// The overwhelmingly common case in this concept (panel shown/hidden, hover grow).
    static float tween_toggle(ImGuiID    owner,
                              ImGuiID    channel,
                              bool       on,
                              float      duration,
                              EasePreset ease,
                              float      dt,
                              bool       init_on);

    /// Per-component tween of an ImVec4 colour. Blends in linear space rather than raw
    /// sRGB so mid-blends do not go muddy — the same reason ImAnim defaults to oklab.
    static ImVec4 tween_color(ImGuiID       owner,
                              ImGuiID       channel,
                              const ImVec4 &target,
                              float         duration,
                              EasePreset    ease,
                              float         dt,
                              const ImVec4 &init);

    /// Advance the frame clock. Call ONCE per frame right after ImGui::NewFrame() —
    /// this is ImAnim's mandatory iam_update_begin_frame() pump. Miss it and every
    /// animated value freezes, which reads as "the animation does nothing".
    static void begin_frame();

    /// Reclaim channels untouched for @p max_idle_frames, so a long-lived UI that
    /// animates transient widgets does not grow the table without bound.
    static void gc(int max_idle_frames = 600);

    /// True when compiled against the real external/ImAnim.
    [[nodiscard]] static bool using_imanim();

    /// Number of live channels — surfaced in the concept's View menu so the keying
    /// model is visible while clicking around (shared ids show up as a suspiciously
    /// low count; leaked ids as a climbing one).
    [[nodiscard]] static int live_channel_count();

private:
    static float apply_ease(EasePreset ease, float t);
};
