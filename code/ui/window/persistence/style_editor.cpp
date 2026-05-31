#include "style_editor.hpp"

namespace {

constexpr const char *kImGuiDefaultPresetName = "ImGui Default";
constexpr const char *kMicaPresetName = "Mica";
constexpr const char *kMicaColorfulPresetName = "Mica Colorful";

WindowStateToml::Vec2Toml ToToml(const ImVec2 &value) {
  WindowStateToml::Vec2Toml result{};
  result.x = value.x;
  result.y = value.y;
  return result;
}

// WindowStateToml::Vec4Toml ToToml(const ImVec4 &value) { // NOLINT

//   WindowStateToml::Vec4Toml result{};
//   result.x = value.x;
//   result.y = value.y;
//   result.z = value.z;
//   result.w = value.w;
//   return result;
// }

WindowStateToml::ColorToml ColorToToml(const ImVec4 &value) {
  auto clamp = [](float v) {
    return static_cast<int>(v * 255.0f + 0.5f < 0     ? 0
                            : v * 255.0f + 0.5f > 255 ? 255
                                                      : v * 255.0f + 0.5f);
  };
  WindowStateToml::ColorToml result{};
  result.r = clamp(value.x);
  result.g = clamp(value.y);
  result.b = clamp(value.z);
  result.a = clamp(value.w);
  return result;
}

ImVec2 FromToml(const WindowStateToml::Vec2Toml &value) {
  return {value.x, value.y};
}

// ImVec4
// FromToml(const WindowStateToml::Vec4Toml
//              &value) // NOLINT(readability-convert-member-functions-to-static)
// {
//   return {value.x, value.y, value.z, value.w};
// }

ImVec4 ColorFromToml(const WindowStateToml::ColorToml &value) {

  const float r = static_cast<float>(value.r) / 255.0f;
  const float g = static_cast<float>(value.g) / 255.0f;
  const float b = static_cast<float>(value.b) / 255.0f;
  const float a = static_cast<float>(value.a) / 255.0f;
  return {r, g, b, a};
}

bool HasPersistedStyle(const WindowStateToml::StyleToml &style) {
  return style.font_size_base > 0.0f && style.curve_tessellation_tol > 0.0f &&
         style.circle_tessellation_max_error > 0.0f && style.alpha > 0.0f;
}

WindowStateToml::StyleToml CaptureStyleToml(const ImGuiStyle &style,
                                            const std::string &preset_name) {
  WindowStateToml::StyleToml result;
  result.preset_name = preset_name;
  result.font_size_base = style.FontSizeBase;
  result.font_scale_main = style.FontScaleMain;
  result.font_scale_dpi = style.FontScaleDpi;
  result.alpha = style.Alpha;
  result.disabled_alpha = style.DisabledAlpha;
  result.window_padding = ToToml(style.WindowPadding);
  result.window_rounding = style.WindowRounding;
  result.window_border_size = style.WindowBorderSize;
  result.window_border_hover_padding = style.WindowBorderHoverPadding;
  result.window_min_size = ToToml(style.WindowMinSize);
  result.window_title_align = ToToml(style.WindowTitleAlign);
  result.window_menu_button_position =
      static_cast<int>(style.WindowMenuButtonPosition);
  result.child_rounding = style.ChildRounding;
  result.child_border_size = style.ChildBorderSize;
  result.popup_rounding = style.PopupRounding;
  result.popup_border_size = style.PopupBorderSize;
  result.frame_padding = ToToml(style.FramePadding);
  result.frame_rounding = style.FrameRounding;
  result.frame_border_size = style.FrameBorderSize;
  result.item_spacing = ToToml(style.ItemSpacing);
  result.item_inner_spacing = ToToml(style.ItemInnerSpacing);
  result.cell_padding = ToToml(style.CellPadding);
  result.touch_extra_padding = ToToml(style.TouchExtraPadding);
  result.indent_spacing = style.IndentSpacing;
  result.columns_min_spacing = style.ColumnsMinSpacing;
  result.scrollbar_size = style.ScrollbarSize;
  result.scrollbar_rounding = style.ScrollbarRounding;
  result.scrollbar_padding = style.ScrollbarPadding;
  result.grab_min_size = style.GrabMinSize;
  result.grab_rounding = style.GrabRounding;
  result.log_slider_deadzone = style.LogSliderDeadzone;
  result.image_rounding = style.ImageRounding;
  result.image_border_size = style.ImageBorderSize;
  result.tab_rounding = style.TabRounding;
  result.tab_border_size = style.TabBorderSize;
  result.tab_min_width_base = style.TabMinWidthBase;
  result.tab_min_width_shrink = style.TabMinWidthShrink;
  result.tab_close_button_min_width_selected =
      style.TabCloseButtonMinWidthSelected;
  result.tab_close_button_min_width_unselected =
      style.TabCloseButtonMinWidthUnselected;
  result.tab_bar_border_size = style.TabBarBorderSize;
  result.tab_bar_overline_size = style.TabBarOverlineSize;
  result.table_angled_headers_angle = style.TableAngledHeadersAngle;
  result.table_angled_headers_text_align =
      ToToml(style.TableAngledHeadersTextAlign);
  result.tree_lines_flags = static_cast<int>(style.TreeLinesFlags);
  result.tree_lines_size = style.TreeLinesSize;
  result.tree_lines_rounding = style.TreeLinesRounding;
  result.drag_drop_target_rounding = style.DragDropTargetRounding;
  result.drag_drop_target_border_size = style.DragDropTargetBorderSize;
  result.drag_drop_target_padding = style.DragDropTargetPadding;
  result.color_marker_size = style.ColorMarkerSize;
  result.color_button_position = static_cast<int>(style.ColorButtonPosition);
  result.button_text_align = ToToml(style.ButtonTextAlign);
  result.selectable_text_align = ToToml(style.SelectableTextAlign);
  result.separator_size = style.SeparatorSize;
  result.separator_text_border_size = style.SeparatorTextBorderSize;
  result.separator_text_align = ToToml(style.SeparatorTextAlign);
  result.separator_text_padding = ToToml(style.SeparatorTextPadding);
  result.display_window_padding = ToToml(style.DisplayWindowPadding);
  result.display_safe_area_padding = ToToml(style.DisplaySafeAreaPadding);
  result.mouse_cursor_scale = style.MouseCursorScale;
  result.anti_aliased_lines = style.AntiAliasedLines;
  result.anti_aliased_lines_use_tex = style.AntiAliasedLinesUseTex;
  result.anti_aliased_fill = style.AntiAliasedFill;
  result.curve_tessellation_tol = style.CurveTessellationTol;
  result.circle_tessellation_max_error = style.CircleTessellationMaxError;
  for (int i = 0; i < ImGuiCol_COUNT; ++i)
    result.colors.at(static_cast<size_t>(i)) = ColorToToml(style.Colors[i]);
  result.hover_stationary_delay = style.HoverStationaryDelay;
  result.hover_delay_short = style.HoverDelayShort;
  result.hover_delay_normal = style.HoverDelayNormal;
  result.hover_flags_for_tooltip_mouse =
      static_cast<int>(style.HoverFlagsForTooltipMouse);
  result.hover_flags_for_tooltip_nav =
      static_cast<int>(style.HoverFlagsForTooltipNav);
  return result;
}

void ApplyStyleToml(ImGuiStyle *style, const WindowStateToml::StyleToml &s) {
  if (!style)
    return;
  style->FontSizeBase = s.font_size_base;
  style->FontScaleMain = s.font_scale_main;
  style->FontScaleDpi = s.font_scale_dpi;
  style->Alpha = s.alpha;
  style->DisabledAlpha = s.disabled_alpha;
  style->WindowPadding = FromToml(s.window_padding);
  style->WindowRounding = s.window_rounding;
  style->WindowBorderSize = s.window_border_size;
  style->WindowBorderHoverPadding =
      std::max(s.window_border_hover_padding, 0.001f);
  {
    const ImVec2 wms = FromToml(s.window_min_size);
    style->WindowMinSize = {std::max(wms.x, 1.0f), std::max(wms.y, 1.0f)};
  }
  style->WindowTitleAlign = FromToml(s.window_title_align);
  {
    const ImGuiDir d = static_cast<ImGuiDir>(s.window_menu_button_position);
    style->WindowMenuButtonPosition =
        (d == ImGuiDir_None || d == ImGuiDir_Left || d == ImGuiDir_Right)
            ? d
            : ImGuiDir_Left;
  }
  style->ChildRounding = s.child_rounding;
  style->ChildBorderSize = s.child_border_size;
  style->PopupRounding = s.popup_rounding;
  style->PopupBorderSize = s.popup_border_size;
  style->FramePadding = FromToml(s.frame_padding);
  style->FrameRounding = s.frame_rounding;
  style->FrameBorderSize = s.frame_border_size;
  style->ItemSpacing = FromToml(s.item_spacing);
  style->ItemInnerSpacing = FromToml(s.item_inner_spacing);
  style->CellPadding = FromToml(s.cell_padding);
  style->TouchExtraPadding = FromToml(s.touch_extra_padding);
  style->IndentSpacing = s.indent_spacing;
  style->ColumnsMinSpacing = s.columns_min_spacing;
  style->ScrollbarSize = s.scrollbar_size;
  style->ScrollbarRounding = s.scrollbar_rounding;
  style->ScrollbarPadding = s.scrollbar_padding;
  style->GrabMinSize = s.grab_min_size;
  style->GrabRounding = s.grab_rounding;
  style->LogSliderDeadzone = s.log_slider_deadzone;
  style->ImageRounding = s.image_rounding;
  style->ImageBorderSize = s.image_border_size;
  style->TabRounding = s.tab_rounding;
  style->TabBorderSize = s.tab_border_size;
  style->TabMinWidthBase = s.tab_min_width_base;
  style->TabMinWidthShrink = s.tab_min_width_shrink;
  style->TabCloseButtonMinWidthSelected = s.tab_close_button_min_width_selected;
  style->TabCloseButtonMinWidthUnselected =
      s.tab_close_button_min_width_unselected;
  style->TabBarBorderSize = s.tab_bar_border_size;
  style->TabBarOverlineSize = s.tab_bar_overline_size;
  style->TableAngledHeadersAngle = s.table_angled_headers_angle;
  style->TableAngledHeadersTextAlign =
      FromToml(s.table_angled_headers_text_align);
  {
    const auto tf = static_cast<ImGuiTreeNodeFlags>(s.tree_lines_flags);
    style->TreeLinesFlags = (tf == ImGuiTreeNodeFlags_DrawLinesNone ||
                             tf == ImGuiTreeNodeFlags_DrawLinesFull ||
                             tf == ImGuiTreeNodeFlags_DrawLinesToNodes)
                                ? tf
                                : ImGuiTreeNodeFlags_DrawLinesNone;
  }
  style->TreeLinesSize = s.tree_lines_size;
  style->TreeLinesRounding = s.tree_lines_rounding;
  style->DragDropTargetRounding = s.drag_drop_target_rounding;
  style->DragDropTargetBorderSize = s.drag_drop_target_border_size;
  style->DragDropTargetPadding = s.drag_drop_target_padding;
  style->ColorMarkerSize = s.color_marker_size;
  {
    const auto d = static_cast<ImGuiDir>(s.color_button_position);
    style->ColorButtonPosition =
        (d == ImGuiDir_Left || d == ImGuiDir_Right) ? d : ImGuiDir_Right;
  }
  style->ButtonTextAlign = FromToml(s.button_text_align);
  style->SelectableTextAlign = FromToml(s.selectable_text_align);
  style->SeparatorSize = s.separator_size;
  style->SeparatorTextBorderSize = s.separator_text_border_size;
  style->SeparatorTextAlign = FromToml(s.separator_text_align);
  style->SeparatorTextPadding = FromToml(s.separator_text_padding);
  style->DisplayWindowPadding = FromToml(s.display_window_padding);
  style->DisplaySafeAreaPadding = FromToml(s.display_safe_area_padding);
  style->MouseCursorScale = s.mouse_cursor_scale;
  style->AntiAliasedLines = s.anti_aliased_lines;
  style->AntiAliasedLinesUseTex = s.anti_aliased_lines_use_tex;
  style->AntiAliasedFill = s.anti_aliased_fill;
  style->CurveTessellationTol = std::max(s.curve_tessellation_tol, 0.001f);
  style->CircleTessellationMaxError =
      std::max(s.circle_tessellation_max_error, 0.001f);
  for (int i = 0; i < ImGuiCol_COUNT; ++i)
    style->Colors[i] = ColorFromToml(s.colors.at(static_cast<size_t>(i)));
  style->HoverStationaryDelay = s.hover_stationary_delay;
  style->HoverDelayShort = s.hover_delay_short;
  style->HoverDelayNormal = s.hover_delay_normal;
  style->HoverFlagsForTooltipMouse =
      static_cast<ImGuiHoveredFlags>(s.hover_flags_for_tooltip_mouse);
  style->HoverFlagsForTooltipNav =
      static_cast<ImGuiHoveredFlags>(s.hover_flags_for_tooltip_nav);
}

} // namespace

StyleEditor::StyleEditor()
    : IsOpen{false}, WindowFlags{ImGuiWindowFlags_None}, m_default_style{},
      m_current_preset_name{}, m_current_font_name{}, m_fonts{},
      m_window_rect{false, 0.0f, 0.0f, 0.0f, 0.0f}, m_apply_layout_once{false} {
}

void StyleEditor::InitDefaults() {
  m_default_style = ImGui::GetStyle();
  m_current_preset_name = kImGuiDefaultPresetName;
}

void StyleEditor::SetFonts(
    std::vector<std::pair<std::string, ImFont *>> fonts) {
  m_fonts = std::move(fonts);
  if (m_fonts.empty())
    return;
  ImFont *current = ImGui::GetIO().FontDefault;
  for (const auto &entry : m_fonts) {
    if (entry.second == current) {
      m_current_font_name = entry.first;
      return;
    }
  }
  m_current_font_name = m_fonts.front().first;
}

void StyleEditor::Draw() {
  if (!IsOpen)
    return;

  if (m_apply_layout_once && m_window_rect.valid) {
    ImGui::SetNextWindowPos(ImVec2(m_window_rect.x, m_window_rect.y),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(m_window_rect.w, m_window_rect.h),
                             ImGuiCond_Always);
  } else {
    ImGui::SetNextWindowSize(ImVec2(720.0f, 680.0f), ImGuiCond_FirstUseEver);
  }

  if (!ImGui::Begin("Style Editor", &IsOpen, WindowFlags)) {
    ImGui::End();
    return;
  }

  {
    ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetWindowSize();
    m_window_rect = {true, pos.x, pos.y, size.x, size.y};
    m_apply_layout_once = false;
  }

  if (!m_fonts.empty()) {
    int font_index = 0;
    for (int i = 0; i < static_cast<int>(m_fonts.size()); ++i) {
      if (m_fonts[static_cast<size_t>(i)].first == m_current_font_name) {
        font_index = i;
        break;
      }
    }
    std::string font_items;
    for (const auto &entry : m_fonts) {
      font_items += entry.first;
      font_items += '\0';
    }
    if (ImGui::Combo("Font", &font_index, font_items.c_str())) {
      m_current_font_name = m_fonts[static_cast<size_t>(font_index)].first;
      ImGui::GetIO().FontDefault =
          m_fonts[static_cast<size_t>(font_index)].second;
    }
  }

  int preset_index = 0;
  if (m_current_preset_name == kMicaPresetName)
    preset_index = 1;
  else if (m_current_preset_name == kMicaColorfulPresetName)
    preset_index = 2;
  if (ImGui::Combo("Preset", &preset_index,
                   "ImGui Default\0Mica\0Mica Colorful\0")) {
    if (preset_index == 1)
      ApplyPresetByName(kMicaPresetName);
    else if (preset_index == 2)
      ApplyPresetByName(kMicaColorfulPresetName);
    else
      ApplyPresetByName(kImGuiDefaultPresetName);
  }

  ImGui::SameLine();
  if (ImGui::Button("Reset To Preset"))
    ApplyPresetByName(m_current_preset_name);

  ImGui::Separator();
  ImGui::ShowStyleEditor();
  ImGui::End();
}

void StyleEditor::ApplyLayout(const WindowStateToml &state) {
  IsOpen = state.show_style_editor_window;
  m_window_rect = state.style_editor_window;
  m_apply_layout_once = m_window_rect.valid;

  if (HasPersistedStyle(state.style)) {
    m_current_preset_name = state.style.preset_name.empty()
                                ? kImGuiDefaultPresetName
                                : state.style.preset_name;
    ApplyStyleToml(&ImGui::GetStyle(), state.style);

    if (!state.style.font_name.empty()) {
      for (const auto &entry : m_fonts) {
        if (entry.first == state.style.font_name) {
          ImGui::GetIO().FontDefault = entry.second;
          m_current_font_name = entry.first;
          break;
        }
      }
    }
  }
}

void StyleEditor::ExportLayout(WindowStateToml *state) const {
  if (!state)
    return;
  state->show_style_editor_window = IsOpen;
  state->style_editor_window = m_window_rect;
  state->style = CaptureStyleToml(ImGui::GetStyle(), m_current_preset_name);
  state->style.font_name = m_current_font_name;
}

void StyleEditor::ApplyPresetByName(const std::string &preset_name) {
  ImGuiStyle &style = ImGui::GetStyle();
  style = m_default_style;
  m_current_preset_name = preset_name;

  if (preset_name == kMicaColorfulPresetName) {
    style.WindowRounding = 12.0f;
    style.ChildRounding = 10.0f;
    style.PopupRounding = 10.0f;
    style.FrameRounding = 8.0f;
    style.GrabRounding = 8.0f;
    style.ScrollbarRounding = 10.0f;
    style.TabRounding = 8.0f;
    style.TabBarOverlineSize = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.ImageBorderSize = 0.0f;
    style.FontSizeBase = 24.0f;
    style.DisabledAlpha = 0.0f;
    style.TreeLinesFlags = static_cast<ImGuiTreeNodeFlags>(262144);

    style.Colors[ImGuiCol_Text] = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.56f, 0.56f, 0.56f, 0.67f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.04f, 0.04f, 0.04f, 0.85f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.07f, 0.07f, 0.07f, 0.12f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.14f, 0.14f, 0.14f, 0.91f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.30f, 0.30f, 0.30f, 0.50f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.39f, 0.39f, 0.39f, 0.99f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.47f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(1.00f, 1.00f, 1.00f, 0.37f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.00f, 0.95f, 0.93f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.25f, 0.25f, 0.25f, 0.34f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.24f, 0.09f, 0.37f, 0.33f);
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.49f, 0.46f, 0.46f, 0.20f);
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.28f, 0.28f, 0.28f, 0.96f);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.60f);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.22f, 0.22f, 0.22f, 0.84f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.32f, 0.32f, 0.32f, 0.92f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.42f, 0.42f, 0.42f, 1.00f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(1.00f, 0.00f, 0.00f, 1.00f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.00f, 0.35f, 1.00f, 1.00f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.69f, 0.69f, 0.69f, 0.50f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(1.00f, 0.95f, 0.95f, 0.92f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.00f, 0.02f, 1.00f, 0.53f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.13f, 0.13f, 0.13f, 0.84f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.11f, 0.03f, 0.98f, 0.92f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.93f, 0.11f, 0.93f, 0.97f);
    style.Colors[ImGuiCol_Separator] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.44f, 0.44f, 0.44f, 0.84f);
    style.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.62f, 0.62f, 0.62f, 0.95f);
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.97f, 0.00f, 1.00f, 0.36f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.46f, 0.46f, 0.46f, 0.74f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.12f, 0.00f, 0.99f, 0.97f);
    style.Colors[ImGuiCol_InputTextCursor] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.15f, 0.15f, 0.15f, 0.93f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.44f, 0.44f, 0.44f, 0.93f);
    style.Colors[ImGuiCol_TabSelected] = ImVec4(1.00f, 0.00f, 0.00f, 0.90f);
    style.Colors[ImGuiCol_TabSelectedOverline] =
        ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    style.Colors[ImGuiCol_TabDimmed] = ImVec4(0.06f, 0.06f, 0.08f, 0.80f);
    style.Colors[ImGuiCol_TabDimmedSelected] =
        ImVec4(0.39f, 0.38f, 0.38f, 0.91f);
    style.Colors[ImGuiCol_TabDimmedSelectedOverline] =
        ImVec4(0.50f, 0.50f, 0.50f, 0.00f);
    style.Colors[ImGuiCol_DockingPreview] = ImVec4(0.98f, 0.26f, 0.87f, 0.36f);
    style.Colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    style.Colors[ImGuiCol_PlotLines] = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.64f, 0.64f, 0.64f, 1.00f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.84f, 0.84f, 0.84f, 1.00f);
    style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.97f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.22f, 0.22f, 0.22f, 0.70f);
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.14f, 0.14f, 0.14f, 0.56f);
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(1.00f, 1.00f, 1.00f, 0.00f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.20f, 0.20f, 0.20f, 0.10f);
    style.Colors[ImGuiCol_TextLink] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.34f, 0.34f, 0.34f, 0.40f);
    style.Colors[ImGuiCol_TreeLines] = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(0.86f, 0.86f, 0.86f, 0.95f);
    style.Colors[ImGuiCol_DragDropTargetBg] =
        ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    style.Colors[ImGuiCol_UnsavedMarker] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    style.Colors[ImGuiCol_NavCursor] = ImVec4(0.80f, 0.80f, 0.80f, 0.95f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(0.74f, 0.74f, 0.74f, 0.76f);
    style.Colors[ImGuiCol_NavWindowingDimBg] =
        ImVec4(0.05f, 0.06f, 0.08f, 0.46f);
    style.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(0.01f, 0.01f, 0.01f, 0.66f);
    return;
  }

  if (preset_name != kMicaPresetName)
    return;

  style.WindowRounding = 12.0f;
  style.ChildRounding = 10.0f;
  style.PopupRounding = 10.0f;
  style.FrameRounding = 8.0f;
  style.GrabRounding = 8.0f;
  style.ScrollbarRounding = 10.0f;
  style.TabRounding = 8.0f;
  style.TabBarOverlineSize = 0.0f;
  style.WindowBorderSize = 1.0f;
  style.ChildBorderSize = 1.0f;
  style.PopupBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;
  style.ImageBorderSize = 0.0f;

  style.Colors[ImGuiCol_Text] = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
  style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.56f, 0.56f, 0.56f, 1.00f);
  style.Colors[ImGuiCol_WindowBg] = ImVec4(0.04f, 0.04f, 0.04f, 0.94f);
  style.Colors[ImGuiCol_ChildBg] = ImVec4(0.07f, 0.07f, 0.07f, 0.80f);
  style.Colors[ImGuiCol_PopupBg] = ImVec4(0.03f, 0.03f, 0.03f, 0.97f);
  style.Colors[ImGuiCol_Border] = ImVec4(0.30f, 0.30f, 0.30f, 0.50f);
  style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  style.Colors[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.10f, 0.10f, 0.86f);
  style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.15f, 0.15f, 0.90f);
  style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.20f, 0.20f, 0.97f);
  style.Colors[ImGuiCol_TitleBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.95f);
  style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.07f, 0.07f, 0.07f, 0.98f);
  style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.04f, 0.04f, 0.04f, 0.78f);
  style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.05f, 0.05f, 0.05f, 0.96f);
  style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.60f);
  style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.22f, 0.22f, 0.22f, 0.84f);
  style.Colors[ImGuiCol_ScrollbarGrabHovered] =
      ImVec4(0.32f, 0.32f, 0.32f, 0.92f);
  style.Colors[ImGuiCol_ScrollbarGrabActive] =
      ImVec4(0.42f, 0.42f, 0.42f, 1.00f);
  style.Colors[ImGuiCol_CheckMark] = ImVec4(0.82f, 0.82f, 0.82f, 1.00f);
  style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.58f, 0.58f, 0.58f, 0.84f);
  style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.74f, 0.74f, 0.74f, 1.00f);
  style.Colors[ImGuiCol_Button] = ImVec4(0.12f, 0.12f, 0.12f, 0.80f);
  style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.18f, 0.18f, 0.92f);
  style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.26f, 0.26f, 0.26f, 0.98f);
  style.Colors[ImGuiCol_Header] = ImVec4(0.13f, 0.13f, 0.13f, 0.84f);
  style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.20f, 0.20f, 0.92f);
  style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.28f, 0.28f, 0.28f, 0.97f);
  style.Colors[ImGuiCol_Separator] = ImVec4(0.24f, 0.24f, 0.24f, 0.50f);
  style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.44f, 0.44f, 0.44f, 0.84f);
  style.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.62f, 0.62f, 0.62f, 0.95f);
  style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.26f, 0.26f, 0.26f, 0.36f);
  style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.46f, 0.46f, 0.46f, 0.74f);
  style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.64f, 0.64f, 0.64f, 0.97f);
  style.Colors[ImGuiCol_Tab] = ImVec4(0.08f, 0.08f, 0.08f, 0.93f);
  style.Colors[ImGuiCol_TabHovered] = ImVec4(0.15f, 0.15f, 0.15f, 0.93f);
  style.Colors[ImGuiCol_TabSelected] = ImVec4(0.12f, 0.12f, 0.12f, 0.99f);
  style.Colors[ImGuiCol_TabDimmed] = ImVec4(0.06f, 0.06f, 0.08f, 0.80f);
  style.Colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.10f, 0.10f, 0.10f, 0.91f);
  style.Colors[ImGuiCol_PlotLines] = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
  style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
  style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.64f, 0.64f, 0.64f, 1.00f);
  style.Colors[ImGuiCol_PlotHistogramHovered] =
      ImVec4(0.84f, 0.84f, 0.84f, 1.00f);
  style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.97f);
  style.Colors[ImGuiCol_TableBorderStrong] = ImVec4(0.22f, 0.22f, 0.22f, 0.70f);
  style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.14f, 0.14f, 0.14f, 0.56f);
  style.Colors[ImGuiCol_TableRowBg] = ImVec4(1.00f, 1.00f, 1.00f, 0.00f);
  style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.20f, 0.20f, 0.20f, 0.10f);
  style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.34f, 0.34f, 0.34f, 0.40f);
  style.Colors[ImGuiCol_DragDropTarget] = ImVec4(0.86f, 0.86f, 0.86f, 0.95f);
  style.Colors[ImGuiCol_NavCursor] = ImVec4(0.80f, 0.80f, 0.80f, 0.95f);
  style.Colors[ImGuiCol_NavWindowingHighlight] =
      ImVec4(0.74f, 0.74f, 0.74f, 0.76f);
  style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.05f, 0.06f, 0.08f, 0.46f);
  style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.01f, 0.01f, 0.01f, 0.66f);
}
