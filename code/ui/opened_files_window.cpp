#include "opened_files_window.hpp"

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

std::optional<WindowStateToml::ImageHistoryEntry> OpenedFilesWindow::draw(const ImageViewerPanel &viewer,
                                                                          HistoryPreview &preview,
                                                                          int *focus_id)
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
                for (const auto &hentry : m_history) {
                    const std::string haystack = hentry.kind + " " + hentry.source;
                    if (!m_filter.PassFilter(haystack.c_str()))
                        continue;

                    const std::string label = "[" + hentry.kind + "] " + hentry.source;
                    if (ImGui::Selectable(label.c_str(), false))
                        activated_entry = hentry;
                    if (ImGui::IsItemHovered())
                        preview.draw_for_hover(hentry);
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
                    else
                        preview.draw_for_hover(make_history_entry(file));
                }
            }
        }
        ImGui::EndChild();
    }

    ImGui::End();
    return activated_entry;
}
