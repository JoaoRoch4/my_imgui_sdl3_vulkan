#pragma once

#include "imgui.h"
#include "window_state_toml.hpp"

#include <functional>

/// Runtime configuration window.
/// Exposes settings that can be changed while the application is running,
/// such as video preview thumbnail dimensions.
class ConfigRuntime {
public:
    ConfigRuntime();

    bool IsOpen;

    /// Draw the configuration window (no-op when IsOpen == false).
    void Draw();

    /// Restore values from persisted state (call after LoadWindowStateToml).
    void ApplyLayout(const WindowStateToml &state);

    /// Write current values into state (call before SaveWindowStateToml).
    void ExportLayout(WindowStateToml *state) const;

    /// Register a callback invoked when the user clicks "Clear Thumbnail Cache".
    void SetClearThumbnailCacheCallback(std::function<void()> cb);

    /// Register a callback invoked when the user clicks "Clear Video Cache".
    void SetClearVideoCacheCallback(std::function<void()> cb);

    /// Register a callback invoked when the user clicks "Rebuild Video Cache".
    void SetRebuildVideoCacheCallback(std::function<void()> cb);

    /// Register a callback invoked when the user clicks "Clear History Metadata".
    void SetClearHistoryMetadataCallback(std::function<void()> cb);

private:
    ImVec2 m_pending_hover_size;
    ImVec2 m_pending_seek_size;
    std::function<void()> m_on_clear_thumbnail_cache;
    std::function<void()> m_on_clear_video_cache;
    std::function<void()> m_on_rebuild_video_cache;
    std::function<void()> m_on_clear_history_metadata;
};
