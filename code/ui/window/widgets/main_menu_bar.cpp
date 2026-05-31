/**
 * @file main_menu_bar.cpp
 * @brief Renders the application main menu bar (File / View) only.
 *
 * All subsystem ownership and per-frame orchestration lives in AppCoordinator.
 * This file is concerned solely with drawing the menu and dispatching the
 * immediate action behind each item, using the subsystems reached through the
 * supplied MenuContext.
 */

#include "pch.hpp"

#include "main_menu_bar.hpp"

#include "FileBrowser.hpp"
#include "app_context.hpp"

#include "Image_viewer_panel.hpp"
#include "config_runtime.hpp"
#include "history_context_menu.hpp"
#include "history_preview.hpp"
#include "media_history_manager.hpp"
#include "open_image_dialogs.hpp"
#include "opened_files_window.hpp"
#include "recent_history_menu.hpp"
#include "style_editor.hpp"
#include "video_context_menu.hpp"
#include "video_playback_mode.hpp"
#include "video_player.hpp"
#include "video_player_placebo.hpp"
#include "window_state_toml.hpp"

bool MainMenuBar::Draw(const MainMenuBar::MenuContext &mc) {
  // Local aliases mirror the historical member names so the menu logic below is
  // unchanged.  Ownership stays in AppContext; these are non-owning observers.
  AppContext &ctx = *mc.ctx;
  auto *m_open_image_dialogs = ctx.OpenImageDialogsPanel();
  auto *m_history_mgr = ctx.History();
  auto *m_history_preview = ctx.Preview();
  auto *m_video_context_menu = ctx.VideoMenu();
  auto *m_opened_files_window = ctx.OpenedFiles();
  auto *m_video_player = ctx.Player();
  auto *m_video_player_placebo = ctx.PlayerPlacebo();
  auto *m_config_runtime = ctx.Config();
  auto *m_viewer = ctx.Viewer();

  StyleEditor *m_style_editor = mc.style_editor;
  bool *m_show_demo_window = mc.show_demo_window;
  bool *m_show_another_window = mc.show_another_window;
  bool &m_show_console = *mc.show_console;
  bool &request_quit = *mc.request_quit;
  const bool m_use_video_player_placebo = mc.use_video_player_placebo;

  if (!ImGui::BeginMainMenuBar())
    return false;

  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Open Image...", "Ctrl+O"))
      m_open_image_dialogs->begin_open_image_dialog();

    if (ImGui::MenuItem("Open Online..."))
      m_open_image_dialogs->open_url_popup();

    auto &history = m_history_mgr->entries();
    if (!history.empty() && ImGui::BeginMenu("Recent")) {
      const auto result = RecentHistoryMenu::draw_entries(
          history,
          {
              .on_open =
                  [&](WindowStateToml::ImageHistoryEntry &entry) {
                    if (entry.kind == "file")
                      m_open_image_dialogs->queue_path(entry.source);
                    else
                      m_open_image_dialogs->queue_url(entry.source);
                  },
              .on_hover =
                  [&](WindowStateToml::ImageHistoryEntry &entry) {
                    m_history_preview->draw_for_hover(entry);
                  },
              .on_after_item =
                  [&](WindowStateToml::ImageHistoryEntry &entry) {
                    const bool is_video =
                        (m_use_video_player_placebo
                             ? VideoPlayerPlacebo::is_video_path(entry.source)
                             : VideoPlayer::is_video_path(entry.source)) ||
                        (m_use_video_player_placebo
                             ? VideoPlayerPlacebo::is_video_url(entry.source)
                             : VideoPlayer::is_video_url(entry.source));

                    if (is_video) {
                      if (const auto r =
                              m_video_context_menu->draw_for_item(entry);
                          r.erase || r.restart_preview || r.quit) {
                        if (r.quit)
                          request_quit = true;
                        if (r.restart_preview) {
                          if (m_use_video_player_placebo)
                            m_video_player_placebo->restart_hover_preview();
                          else
                            m_video_player->restart_hover_preview();
                        }
                        if (r.erase) {
                          m_history_mgr->erase(r.erase_source,
                                               *m_opened_files_window);
                          ImGui::EndMenu();
                          ImGui::EndMenu();
                          return true;
                        }
                      }
                      return false;
                    }

                    if (const auto erase =
                            HistoryContextMenu::draw_for_item(entry.source)) {
                      m_history_mgr->erase(*erase, *m_opened_files_window);
                      ImGui::EndMenu();
                      ImGui::EndMenu();
                      return true;
                    }
                    return false;
                  },
          });

      if (result.shown < static_cast<int>(history.size())) {
        ImGui::Separator();
        ImGui::TextDisabled("(%zu more not shown)",
                            history.size() - static_cast<size_t>(result.shown));
      }

      if (!result.stopped) {
        ImGui::Separator();
        if (ImGui::MenuItem("Clear History"))
          m_history_mgr->clear(*m_opened_files_window);
        ImGui::EndMenu();
      }
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Quit", "Alt+F4"))
      request_quit = true;

    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("View")) {
    auto &show_file_explorer = *mc.show_file_explorer;
    auto &file_explorer = *mc.file_explorer;

    if (ImGui::MenuItem("File Explorer", nullptr, show_file_explorer)) {
      show_file_explorer = !show_file_explorer;
      if (show_file_explorer)
        file_explorer.Open();
      else
        file_explorer.Close();
    }

    if (ImGui::MenuItem("Console", nullptr, m_show_console))
      m_show_console = !m_show_console;

    if (m_style_editor)
      ImGui::MenuItem("Style Editor", nullptr, &m_style_editor->IsOpen);

    if (m_show_demo_window)
      ImGui::MenuItem("Demo Window", nullptr, m_show_demo_window);

    if (m_show_another_window)
      ImGui::MenuItem("Another Window", nullptr, m_show_another_window);

    ImGui::MenuItem("Opened Files", nullptr, &m_opened_files_window->IsOpen);
    ImGui::MenuItem("Runtime Config", nullptr, &m_config_runtime->IsOpen);

    if (m_viewer->count() > 0) {
      ImGui::Separator();
      m_viewer->build_view_menu_items();
    }

    ImGui::EndMenu();
  }

  ImGui::EndMainMenuBar();
  return true;
}
