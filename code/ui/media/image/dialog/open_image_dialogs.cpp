#include "pch.hpp"

#include "open_image_dialogs.hpp"

#include <SDL3/SDL_dialog.h>

OpenImageDialogs::OpenImageDialogs()
    : m_window{nullptr}, m_pending_paths{}, m_pending_urls{},
      m_show_url_popup{false}, m_focus_url_input{false}, m_url_buf{} {}

void OpenImageDialogs::setup(SDL_Window *window) { m_window = window; }

void OpenImageDialogs::begin_open_image_dialog() {
  static const SDL_DialogFileFilter filters[] = {
      {"All media", "png;jpg;jpeg;bmp;tga;webp;gif;mp4;mkv;avi;mov;webm;flv;"
                    "wmv;m4v;mp3;flac;ogg;wav;aac;opus"},

      {"Image files", "png;jpg;jpeg;bmp;tga;gif;webp"},
      {"Video files", "mp4;mkv;avi;mov;webm;flv;wmv;m4v;ts;mpeg;mpg;ogv;3gp;rm;"
                      "rmvb;divx;xvid"},
      {"Audio files", "mp3;flac;ogg;wav;aac;opus;m4a;wma;ac3;dts"},
      {"All files", "*"},
  };

  SDL_ShowOpenFileDialog(file_dialog_callback, this, m_window, filters, 5,
                         nullptr, true);
}

void OpenImageDialogs::open_url_popup() {
  m_url_buf[0] = '\0';
  m_show_url_popup = true;
  m_focus_url_input = true;
}

void OpenImageDialogs::draw_url_popup() {
  if (m_show_url_popup) {
    ImGui::OpenPopup("Open Online");
    m_show_url_popup = false;
  }

  if (!ImGui::BeginPopupModal("Open Online", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize))
    return;

  ImGui::Text("Enter image URL:");
  ImGui::SetNextItemWidth(480.0f);
  if (m_focus_url_input) {
    ImGui::SetKeyboardFocusHere();
    m_focus_url_input = false;
  }
  const bool enter_pressed =
      ImGui::InputText("##url", m_url_buf.data(), m_url_buf.size(),
                       ImGuiInputTextFlags_EnterReturnsTrue);

  ImGui::SetItemDefaultFocus();

  const bool ok = ImGui::Button("Open", ImVec2(100.0f, 0.0f)) || enter_pressed;
  ImGui::SameLine();
  const bool cancel = ImGui::Button("Cancel", ImVec2(100.0f, 0.0f));

  if (ok && m_url_buf[0] != '\0') {
    m_pending_urls.emplace_back(m_url_buf.data());
    m_url_buf[0] = '\0';
    ImGui::CloseCurrentPopup();
  } else if (cancel || (enter_pressed && m_url_buf[0] == '\0')) {
    ImGui::CloseCurrentPopup();
  }

  ImGui::EndPopup();
}

void OpenImageDialogs::queue_path(const std::string &path) {
  m_pending_paths.push_back(path);
}

void OpenImageDialogs::queue_url(const std::string &url) {
  m_pending_urls.push_back(url);
}

std::vector<std::string> OpenImageDialogs::take_pending_paths() {
  std::vector<std::string> paths = std::move(m_pending_paths);
  m_pending_paths.clear();
  return paths;
}

std::vector<std::string> OpenImageDialogs::take_pending_urls() {
  std::vector<std::string> urls = std::move(m_pending_urls);
  m_pending_urls.clear();
  return urls;
}

void OpenImageDialogs::file_dialog_callback(void *userdata,
                                            const char *const *filelist,
                                            int /*filter*/) {
  auto *self = static_cast<OpenImageDialogs *>(userdata);

  if (filelist == nullptr)
    return;

  for (int i = 0; filelist[i] != nullptr; ++i)
    self->m_pending_paths.emplace_back(filelist[i]);
}
