#pragma once

/// Easing curves available to Motion::tween_*.
///
/// Names mirror the ImAnim presets the project's `imanim` skill documents
/// (iam_ease_out_cubic, iam_ease_out_quad, iam_ease_back, iam_ease_elastic) so the
/// mapping in motion.cpp's CONCEPT_HAS_IMANIM branch is one-to-one and obvious.
enum class EasePreset {
    Linear,     ///< no shaping — for values that must track input exactly
    OutQuad,    ///< gentle settle; good for colour and alpha cross-fades
    OutCubic,   ///< the workhorse: panel slides, hover grows
    OutBack,    ///< slight overshoot past the target, then settles
    OutElastic, ///< springy overshoot; use sparingly, it draws the eye
};
