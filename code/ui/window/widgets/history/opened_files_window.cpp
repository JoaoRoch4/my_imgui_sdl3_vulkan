#include "opened_files_window.hpp"
#include "history_context_menu.hpp"
#include "history_preview.hpp"
#include "video_context_menu.hpp"
#include "video_player.hpp"



namespace {

static WindowStateToml::ImageHistoryEntry make_history_entry(const ImageViewerPanel::OpenedFileInfo &file) { // NOLINT
    return WindowStateToml::ImageHistoryEntry{file.source, file.kind, ""};
}

} // namespace

OpenedFilesWindow::OpenedFilesWindow()
    : IsOpen{false}
    , m_filter{}
    , m_history{}
    , m_on_erase_entry{nullptr}
    , m_on_restart_preview{nullptr}
    , m_on_rescan_toml{nullptr}
    , m_on_open_image{nullptr}
    , m_on_open_online{nullptr}
    , m_on_fix_videos{nullptr}
    , m_is_startup_videos_fixed{nullptr} {
}

bool OpenedFilesWindow::load_history_from_toml(const std::filesystem::path &file_path) {
    WindowStateToml state;
    if (!LoadWindowStateToml(file_path, state))
        return false;

    apply_history(state);
    return true;
}

void OpenedFilesWindow::apply_history(const WindowStateToml &state) {
    m_history = state.image_history;
}

void OpenedFilesWindow::ApplyLayout(const WindowStateToml &state) {
    IsOpen = state.show_opened_files_window;
}

void OpenedFilesWindow::ExportLayout(WindowStateToml *state) const {
    state->show_opened_files_window = IsOpen;
}

void OpenedFilesWindow::sync_history(const std::vector<WindowStateToml::ImageHistoryEntry> &history) {
    m_history = history;
}

void OpenedFilesWindow::SetEraseHistoryEntryCallback(std::function<void(const std::string &)> cb) {
    m_on_erase_entry = std::move(cb);
}

void OpenedFilesWindow::SetRestartPreviewCallback(std::function<void()> cb) {
    m_on_restart_preview = std::move(cb);
}

void OpenedFilesWindow::SetRescanTomlCallback(std::function<void()> cb) {
    m_on_rescan_toml = std::move(cb);
}

void OpenedFilesWindow::SetMenuShortcutsCallbacks(std::function<void()> on_open_image,
                                                  std::function<void()> on_open_online,
                                                  std::function<void()> on_fix_videos,
                                                  std::function<bool()> is_startup_videos_fixed) {
    m_on_open_image = std::move(on_open_image);
    m_on_open_online = std::move(on_open_online);
    m_on_fix_videos = std::move(on_fix_videos);
    m_is_startup_videos_fixed = std::move(is_startup_videos_fixed);
}

void OpenedFilesWindow::SetQuitCallback(std::function<void()> cb) {
    m_on_quit = std::move(cb);
}

std::optional<WindowStateToml::ImageHistoryEntry> OpenedFilesWindow::draw(const ImageViewerPanel &viewer,
                                                                          HistoryPreview &preview,
                                                                          int *focus_id,
                                                                          VideoContextMenu *video_ctx) {
    if (!IsOpen)
        return std::nullopt;

    if (!ImGui::Begin("Opened Files", &IsOpen, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return std::nullopt;
    }

    if (ImGui::BeginMenuBar()) {
        const bool startup_fixed = m_is_startup_videos_fixed && m_is_startup_videos_fixed();
        const char *startup_label = startup_fixed ? "Unfix Startup Videos" : "Set Startup Videos";

        if (ImGui::BeginMenu("File")) {
            if (m_on_open_image && ImGui::MenuItem("Open Image...", "Ctrl+O"))
                m_on_open_image();
            if (m_on_open_online && ImGui::MenuItem("Open Online..."))
                m_on_open_online();
            if (m_on_fix_videos && ImGui::MenuItem(startup_label))
                m_on_fix_videos();
            ImGui::Separator();
            if (ImGui::MenuItem("Rescan TOML") && m_on_rescan_toml)
                m_on_rescan_toml();
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    const auto files = viewer.opened_files();
    std::vector<ImageViewerPanel::OpenedFileInfo> ordered;
    ordered.reserve(files.size());

    std::vector<bool> used(files.size(), false);

    for (const auto &hentry : m_history) {
        for (size_t i = 0; i < files.size(); ++i) {
            if (used[i])
                continue;

            if (files[i].source == hentry.source && files[i].kind == hentry.kind) {
                ordered.push_back(files[i]);
                used[i] = true;
                break;
            }
        }
    }

    for (size_t i = 0; i < files.size(); ++i) {
        if (!used[i])
            ordered.push_back(files[i]);
    }

    m_filter.Draw("Search", 260.0f);
    ImGui::SameLine();
    if (ImGui::Button("Rescan TOML") && m_on_rescan_toml)
        m_on_rescan_toml();

    int filtered_count = 0;
    for (const auto &file : ordered) {
        const std::string haystack = file.kind + " " + file.title + " " + file.source;
        if (m_filter.PassFilter(haystack.c_str()))
            ++filtered_count;
    }

    ImGui::Text("Opened files: %d", static_cast<int>(ordered.size()));
    ImGui::SameLine();
    ImGui::TextDisabled("Filtered: %d", filtered_count);
    ImGui::TextDisabled("Synced with Recent history order (from TOML)");
    ImGui::Separator();

    std::optional<WindowStateToml::ImageHistoryEntry> activated_entry;

    if (ordered.empty()) {
        ImGui::TextDisabled("No opened image windows.");
        if (!m_history.empty()) {
            ImGui::Spacing();
            ImGui::Text("History from TOML:");
            ImGui::Separator();
            if (ImGui::BeginChild("##history_from_toml_list", ImVec2(0.0f, 0.0f), true)) {
                for (auto &hentry : m_history) {
                    const std::string haystack = hentry.kind + " " + hentry.source;
                    if (!m_filter.PassFilter(haystack.c_str()))
                        continue;

                    std::string display;
                    if (hentry.kind == "file") {
                        display = std::filesystem::path(hentry.source).filename().string();
                    } else {
                        display = !hentry.title.empty() ? hentry.title : hentry.source;
                        if (display.size() > 80)
                            display = display.substr(0, 77) + "...";
                    }
                    const std::string label = "[" + hentry.kind + "] " + display;
                    if (ImGui::Selectable(label.c_str(), false))
                        activated_entry = hentry;
                    if (ImGui::IsItemHovered())
                        preview.draw_for_hover(hentry);
                    const bool is_video = VideoPlayer::is_video_path(hentry.source) ||
                                          VideoPlayer::is_video_url(hentry.source);
                    if (is_video && video_ctx) {
                        if (const auto r = video_ctx->draw_for_item(hentry); r.erase || r.restart_preview || r.quit) {
                            if (r.erase && m_on_erase_entry)
                                m_on_erase_entry(r.erase_source);
                            if (r.restart_preview && m_on_restart_preview)
                                m_on_restart_preview();
                            if (r.quit && m_on_quit)
                                m_on_quit();
                        }
                    } else {
                        if (const auto erase = HistoryContextMenu::draw_for_item(hentry.source))
                            if (m_on_erase_entry)
                                m_on_erase_entry(*erase);
                    }
                }
            }
            ImGui::EndChild();
        }
    } else {
        if (ImGui::BeginChild("##opened_files_list", ImVec2(0.0f, 0.0f), true)) {
            for (const auto &file : ordered) {
                const std::string haystack = file.kind + " " + file.title + " " + file.source;
                if (!m_filter.PassFilter(haystack.c_str()))
                    continue;

                const std::string label = "[" + file.kind + "] " + file.title + "###opened_" + std::to_string(file.id);
                if (ImGui::Selectable(label.c_str(), false) && focus_id)
                    *focus_id = file.id;

                const auto it = std::find_if(m_history.begin(), m_history.end(), [&file](const WindowStateToml::ImageHistoryEntry &hentry) {
                    return hentry.source == file.source && hentry.kind == file.kind;
                });

                if (ImGui::IsItemHovered()) {
                    if (it != m_history.end())
                        preview.draw_for_hover(*it);
                    else {
                        auto temp_entry = make_history_entry(file);
                        preview.draw_for_hover(temp_entry);
                    }
                }
                const bool is_video_file = VideoPlayer::is_video_path(file.source) ||
                                           VideoPlayer::is_video_url(file.source);
                if (is_video_file && video_ctx && it != m_history.end()) {
                    if (const auto r = video_ctx->draw_for_item(*it); r.erase || r.restart_preview || r.quit) {
                        if (r.erase && m_on_erase_entry)
                            m_on_erase_entry(r.erase_source);
                        if (r.restart_preview && m_on_restart_preview)
                            m_on_restart_preview();
                        if (r.quit && m_on_quit)
                            m_on_quit();
                    }
                } else {
                    if (const auto erase = HistoryContextMenu::draw_for_item(file.source))
                        if (m_on_erase_entry)
                            m_on_erase_entry(*erase);
                }
            }
        }
        ImGui::EndChild();
    }

    ImGui::End();
    return activated_entry;
}
