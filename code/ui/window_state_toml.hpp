#pragma once

#include "imgui.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct WindowStateToml
{
    struct WindowRectToml
    {
        bool  valid = false;
        float x     = 0.0f;
        float y     = 0.0f;
        float w     = 0.0f;
        float h     = 0.0f;
    };

    struct Vec2Toml
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct Vec4Toml
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 1.0f;
    };

    struct ColorToml
    {
        int r = 0;
        int g = 0;
        int b = 0;
        int a = 255;
    };

    struct StyleToml
    {
        std::string preset_name;
        std::string font_name;
        float font_size_base;
        float font_scale_main;
        float font_scale_dpi;
        float alpha;
        float disabled_alpha;
        Vec2Toml window_padding;
        float window_rounding;
        float window_border_size;
        float window_border_hover_padding;
        Vec2Toml window_min_size;
        Vec2Toml window_title_align;
        int window_menu_button_position;
        float child_rounding;
        float child_border_size;
        float popup_rounding;
        float popup_border_size;
        Vec2Toml frame_padding;
        float frame_rounding;
        float frame_border_size;
        Vec2Toml item_spacing;
        Vec2Toml item_inner_spacing;
        Vec2Toml cell_padding;
        Vec2Toml touch_extra_padding;
        float indent_spacing;
        float columns_min_spacing;
        float scrollbar_size;
        float scrollbar_rounding;
        float scrollbar_padding;
        float grab_min_size;
        float grab_rounding;
        float log_slider_deadzone;
        float image_rounding;
        float image_border_size;
        float tab_rounding;
        float tab_border_size;
        float tab_min_width_base;
        float tab_min_width_shrink;
        float tab_close_button_min_width_selected;
        float tab_close_button_min_width_unselected;
        float tab_bar_border_size;
        float tab_bar_overline_size;
        float table_angled_headers_angle;
        Vec2Toml table_angled_headers_text_align;
        int tree_lines_flags;
        float tree_lines_size;
        float tree_lines_rounding;
        float drag_drop_target_rounding;
        float drag_drop_target_border_size;
        float drag_drop_target_padding;
        float color_marker_size;
        int color_button_position;
        Vec2Toml button_text_align;
        Vec2Toml selectable_text_align;
        float separator_size;
        float separator_text_border_size;
        Vec2Toml separator_text_align;
        Vec2Toml separator_text_padding;
        Vec2Toml display_window_padding;
        Vec2Toml display_safe_area_padding;
        float mouse_cursor_scale;
        bool anti_aliased_lines;
        bool anti_aliased_lines_use_tex;
        bool anti_aliased_fill;
        float curve_tessellation_tol;
        float circle_tessellation_max_error;
        std::array<ColorToml, ImGuiCol_COUNT> colors;
        float hover_stationary_delay;
        float hover_delay_short;
        float hover_delay_normal;
        int hover_flags_for_tooltip_mouse;
        int hover_flags_for_tooltip_nav;
    };

    // One entry per image or URL opened by the user. Newest entry first.
    struct ImageHistoryEntry
    {
        std::string source;     // absolute file path or URL
        std::string kind;       // "file" | "url"
        std::string opened_at;  // ISO 8601 timestamp (yyyy-mm-ddTHH:MM:SS)
    };

    bool show_demo_window         = true;
    bool show_another_window      = false;
    bool show_style_editor_window = false;
    bool vsync                    = true;

    std::optional<ColorToml> clear_color;

    WindowRectToml hello_world_window;
    WindowRectToml another_window;
    WindowRectToml style_editor_window;
    StyleToml      style;
    std::vector<ImageHistoryEntry> image_history;  // newest first, persisted across runs
};

bool LoadWindowStateToml(const std::filesystem::path& file_path, WindowStateToml& state);
bool SaveWindowStateToml(const std::filesystem::path& file_path, const WindowStateToml& state);
