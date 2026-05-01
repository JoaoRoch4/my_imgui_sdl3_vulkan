#pragma once

#include "imgui.h"
#include "window_state_toml.hpp"

#include <string>
#include <utility>
#include <vector>

// StyleEditor owns the Dear ImGui style preset selection, preset switching
// logic, and the full ImGuiStyle TOML persistence.
class StyleEditor
{
public:
    StyleEditor();

    bool             IsOpen;
    ImGuiWindowFlags WindowFlags;

    // Call immediately after ImGui init to capture the default style.
    void InitDefaults();

    // Register the available fonts (name → pointer). Call after ImGui font loading.
    void SetFonts(std::vector<std::pair<std::string, ImFont*>> fonts);

    // Draw the style editor window (no-op when IsOpen == false).
    void Draw();

    // Restore / capture persisted layout.
    void ApplyLayout(const WindowStateToml& state);
    void ExportLayout(WindowStateToml* state) const;

private:
    void ApplyPresetByName(const std::string& preset_name);

    ImGuiStyle                                   m_default_style;
    std::string                                  m_current_preset_name;
    std::string                                  m_current_font_name;
    std::vector<std::pair<std::string, ImFont*>> m_fonts;
    WindowStateToml::WindowRectToml              m_window_rect;
    bool                                         m_apply_layout_once;
};
