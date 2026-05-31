#pragma once

#include <array>

// Playback mode selector shared across Runtime Config and Video Context Menu.
enum class VideoPlaybackMode : int {
    SwMpv = 0,
    NvdecMpv = 1,
    NvdecLibplacebo = 2,
};

inline int sanitize_video_playback_mode(int mode) {
    if (mode < static_cast<int>(VideoPlaybackMode::SwMpv) ||
        mode > static_cast<int>(VideoPlaybackMode::NvdecLibplacebo)) {
        return static_cast<int>(VideoPlaybackMode::SwMpv);
    }
    return mode;
}

inline bool mode_uses_hwdec(int mode) {
    return sanitize_video_playback_mode(mode) != static_cast<int>(VideoPlaybackMode::SwMpv);
}

inline bool mode_uses_libplacebo(int mode) {
    return sanitize_video_playback_mode(mode) == static_cast<int>(VideoPlaybackMode::NvdecLibplacebo);
}

inline constexpr std::array<const char *, 3> k_video_playback_mode_items = {
    "SW MPV",
    "NVDEC MPV",
    "NVDEC libplacebo",
};
