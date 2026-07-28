#pragma once

#include "pch.hpp"

/// One animated scalar, owned by Motion's channel table and keyed by
/// (owner ImGuiID, channel ImGuiID) — ImAnim's keying model.
///
/// `from`/`to` are re-seeded whenever the caller passes a new target, with `from` set
/// to the CURRENT value rather than the old target. That is what makes a target flip
/// mid-flight redirect smoothly instead of snapping — ImAnim calls it the crossfade
/// policy (iam_policy_crossfade), and it is the only policy this concept needs.
struct MotionChannel {
    float from     = 0.0f; ///< value when the current leg started
    float to       = 0.0f; ///< target the current leg is heading for
    float current  = 0.0f; ///< value returned to the caller this frame
    float elapsed  = 0.0f; ///< seconds into the current leg
    float duration = 0.0f; ///< seconds the current leg takes
    int   used_frame = 0;  ///< ImGui frame this channel was last touched (for gc)
};
