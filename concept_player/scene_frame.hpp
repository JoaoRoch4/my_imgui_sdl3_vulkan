#pragma once

#include "pch.hpp"

/// One entry in the scene view — the sketch's "+5 sec frame".
///
/// In the real player each of these is a decoded frame lifted out of the source at
/// `timestamp` (the existing VideoSeekPreview already does exactly this for one
/// timestamp at a time; the strip is N of them). Here the picture is generated, but
/// the shape of the data — a timestamp plus a pre-formatted label — is what the real
/// implementation would carry, so the widget code ports across unchanged.
struct SceneFrame {
    double      timestamp = 0.0; ///< seconds into the clip
    std::string label;           ///< pre-formatted "m:ss", built once when the strip is built
};
