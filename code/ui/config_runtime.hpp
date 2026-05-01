#pragma once

#include "imgui.h"
#include "window_state_toml.hpp"

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

private:
    ImVec2 m_pending_hover_size;
    ImVec2 m_pending_seek_size;
};
