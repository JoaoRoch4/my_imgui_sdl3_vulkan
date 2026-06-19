#pragma once
#include "pch.hpp"

class StyleEditor;
namespace ImGui {
class FileBrowser;
}

/**
 * Renders the application's main menu bar (the File and View menus) and
 * dispatches the immediate action for each item.
 *
 * MainMenuBar holds no state: everything it needs is supplied per-frame through
 * MenuContext.  Subsystems are owned by the MemoryManagement registry; Draw()
 * reads them via GetInstance<T>() to build the menu.
 */
class MainMenuBar {
public:
  /// Per-frame inputs for drawing the menu bar.
  struct MenuContext {
    StyleEditor *style_editor = nullptr;
    bool *show_demo_window = nullptr;   ///< toggled by "Demo Window"
    bool *show_another_window = nullptr;
    bool *show_console = nullptr;       ///< toggled by "Console"
    bool *show_file_explorer = nullptr; ///< toggled by "File Explorer"
    ImGui::FileBrowser *file_explorer = nullptr; ///< Open()/Close() target
    bool *request_quit = nullptr;       ///< set true by "Quit" / context menu
    bool use_video_player_placebo = false;
  };

  /// Draw the menu bar.  Returns false when BeginMainMenuBar() fails; callers
  /// should then skip the rest of their per-frame UI (historical behaviour).
  bool Draw(const MenuContext &mc);
};
