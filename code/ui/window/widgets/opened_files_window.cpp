#include "opened_files_window.hpp"
#include "history_context_menu.hpp"
#include "history_preview.hpp"
#include "video_context_menu.hpp"
#include "video_player.hpp"

#include "imgui.h"

#include <algorithm>

namespace {

static WindowStateToml::ImageHistoryEntry make_history_entry(const ImageViewerPanel::OpenedFileInfo &file)
{
    return WindowStateToml::ImageHistoryEntry{file.source, file.kind, ""};
}

} // namespace

OpenedFilesWindow::OpenedFilesWindow()
    : IsOpen{false}
    , m_filter{}
    , m_history{}
    , m_on_erase_entry{nullptr}
{
}

bool OpenedFilesWindow::load_history_from_toml(const std::filesystem::path &file_path)
{
    WindowStateToml state;
    if (!LoadWindowStateToml(file_path, state))
        return false;

    apply_history(state);
    return true;
}

void OpenedFilesWindow::apply_history(const WindowStateToml &state)
{
    m_history = state.image_history;
}

void OpenedFilesWindow::sync_history(const std::vector<WindowStateToml::ImageHistoryEntry> &history)
{
    m_history = history;
}

void OpenedFilesWindow::SetEraseHistoryEntryCallback(std::function<void(const std::string &)> cb)
{
    m_on_erase_entry = std::move(cb);
}

std::optional<WindowStateToml::ImageHistoryEntry> OpenedFilesWindow::draw(const ImageViewerPanel &viewer,
                                                                          HistoryPreview &preview,
                                                                          int *focus_id,
                                                                          VideoContextMenu *video_ctx)
{
    if (!IsOpen)
        return std::nullopt;

    if (!ImGui::Begin("Opened Files", &IsOpen)) {
        ImGui::End();
        return std::nullopt;
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

                    const std::string label = "[" + hentry.kind + "] " + hentry.source;
                    if (ImGui::Selectable(label.c_str(), false))
                        activated_entry = hentry;
                    if (ImGui::IsItemHovered())
                        preview.draw_for_hover(hentry);
                    const bool is_video = VideoPlayer::is_video_path(hentry.source) ||
                                         VideoPlayer::is_video_url(hentry.source);
                    if (is_video && video_ctx) {
                        if (const auto r = video_ctx->draw_for_item(hentry); r.erase)
                            if (m_on_erase_entry) m_on_erase_entry(r.erase_source);
                    } else {
                        if (const auto erase = HistoryContextMenu::draw_for_item(hentry.source))
                            if (m_on_erase_entry) m_on_erase_entry(*erase);
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
                    if (const auto r = video_ctx->draw_for_item(*it); r.erase)
                        if (m_on_erase_entry) m_on_erase_entry(r.erase_source);
                } else {
                    if (const auto erase = HistoryContextMenu::draw_for_item(file.source))
                        if (m_on_erase_entry) m_on_erase_entry(*erase);
                }
            }
        }
        ImGui::EndChild();
    }

    ImGui::End();
    return activated_entry;
}
