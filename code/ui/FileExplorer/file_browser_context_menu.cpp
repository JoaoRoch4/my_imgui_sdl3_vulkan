#include "file_browser_context_menu.hpp"

#include "imgui.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

// ---- helpers ---------------------------------------------------------------

namespace {

void open_in_file_manager(const std::filesystem::path &dir) {
  // xdg-open is the standard "open in default app" on Linux desktops.
  const std::string cmd = "xdg-open " + dir.string() + " &";
  std::system(cmd.c_str()); // NOLINT(cert-env33-c)
}

} // namespace

// ---- FileBrowserContextMenu ------------------------------------------------

FileBrowserContextMenu::FileBrowserContextMenu() = default;

void FileBrowserContextMenu::setup(SDL_Window *window) { m_window = window; }

void FileBrowserContextMenu::SetExtraItemsCallback(
    std::function<void(const std::filesystem::path &)> cb) {
  m_extra_items_cb = std::move(cb);
}

// ---------------------------------------------------------------------------
// draw() — call immediately after the file item's Selectable, once per item.
// ---------------------------------------------------------------------------

FileBrowserContextMenu::Result
FileBrowserContextMenu::draw(const std::filesystem::path &path) {
  // The popup id is per-item so each file gets its own popup.
  const std::string popup_id = "##fb_ctx_" + path.string();
  if (!ImGui::BeginPopupContextItem(popup_id.c_str()))
    return {};

  Result result = draw_menu_items(path);
  ImGui::EndPopup();
  return result;
}

// ---------------------------------------------------------------------------
// draw_menu_items() — inner items, no Begin/EndPopup wrapper.
// ---------------------------------------------------------------------------

FileBrowserContextMenu::Result
FileBrowserContextMenu::draw_menu_items(const std::filesystem::path &path) {
  Result result;

  // Header: show filename, dimmed.
  ImGui::TextDisabled("%s", path.filename().string().c_str());
  ImGui::Separator();

  if (ImGui::MenuItem("Open")) {
    result.open = true;
    result.open_path = path;
  }

  if (ImGui::MenuItem("Copy Path")) {
    ImGui::SetClipboardText(path.string().c_str());
  }

  ImGui::Separator();

  if (ImGui::MenuItem("Open Containing Folder")) {
    open_in_file_manager(path.parent_path());
  }

  ImGui::Separator();

  if (ImGui::MenuItem("Delete\xe2\x80\xa6")) { // "Delete…" in UTF-8
    m_pending_delete_path = path;
    m_want_open_modal = true;
  }
  if (m_extra_items_cb) {
    ImGui::Separator();
    m_extra_items_cb(path);
  }
  return result;
}

// ---------------------------------------------------------------------------
// process_pending() — call once per frame from the main render loop.
// ---------------------------------------------------------------------------

void FileBrowserContextMenu::process_pending() {
  // If a delete was just requested, ask ImGui to open the modal next frame.
  if (m_want_open_modal) {
    ImGui::OpenPopup("Delete File?##fb_del_modal");
    m_want_open_modal = false;
  }

  // Always attempt to render the modal (it is a no-op when not open).
  const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {0.5f, 0.5f});

  if (ImGui::BeginPopupModal("Delete File?##fb_del_modal", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Delete file:");
    ImGui::Spacing();
    ImGui::TextWrapped("%s", m_pending_delete_path.string().c_str());
    ImGui::Spacing();
    ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f},
                       "This action cannot be undone.");
    ImGui::Separator();

    if (ImGui::Button("Delete", {120, 0})) {
      std::error_code ec;
      std::filesystem::remove(m_pending_delete_path, ec);
      // Report via m_last_deleted; callers may inspect after
      // process_pending() if needed (kept simple: no callback).
      m_pending_delete_path.clear();
      ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();

    if (ImGui::Button("Cancel", {120, 0})) {
      m_pending_delete_path.clear();
      ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
  }
}
