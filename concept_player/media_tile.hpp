#pragma once

#include "pch.hpp"

/// One item in the folder view.
///
/// The concept has no decoder, so a "clip" is just a seed plus a duration: the seed
/// picks the palette and the shapes FramePattern draws, the duration sets the length
/// of the timeline. That is enough for the interaction being designed — a folder of
/// visually distinct things you can hover, preview, and switch between.
struct MediaTile {
    std::string name;
    std::uint32_t seed     = 0;
    double        duration = 0.0; ///< seconds
    bool          is_video = true;
};
