#include "video_context_menu.hpp"

#include "video_player.hpp"

#include "imgui.h"
#include <SDL3/SDL_dialog.h>

#include <filesystem>
#include <print>

// ---------------------------------------------------------------------------
// Constructor / setup
// ---------------------------------------------------------------------------

VideoContextMenu::VideoContextMenu()
    : m_window{nullptr}
    , m_copy_source{}
    , m_copy_dest{}
{
}

void VideoContextMenu::setup(SDL_Window *window)
{
    m_window = window;
}

// ---------------------------------------------------------------------------
// Save-file dialog callback (called on the main thread by SDL3)
// ---------------------------------------------------------------------------

void VideoContextMenu::save_dialog_callback(void *userdata,
                                             const char *const *filelist,
                                             int /*filter*/)
{
    auto *self = static_cast<VideoContextMenu *>(userdata);
    if (!filelist || !filelist[0]) {
        self->m_copy_source.clear(); // user cancelled
        return;
    }
    self->m_copy_dest = std::filesystem::path(filelist[0]);
}

// ---------------------------------------------------------------------------
// Shared menu body (used by both draw_for_item and draw_for_window)
// ---------------------------------------------------------------------------

static VideoContextMenu::Result draw_menu_body(
    VideoContextMenu *self,
    SDL_Window *window,
    std::filesystem::path &copy_source,
    std::filesystem::path &copy_dest,
    const WindowStateToml::ImageHistoryEntry &entry)
{
    VideoContextMenu::Result result;

    // Short preview label.
    const std::string preview = entry.source.size() > 64
        ? entry.source.substr(0, 61) + "..."
        : entry.source;
    ImGui::TextDisabled("%s", preview.c_str());
    ImGui::Separator();

    // ----- Remove from History --------------------------------------------
    if (ImGui::MenuItem("Remove from History")) {
        result.erase        = true;
        result.erase_source = entry.source;
    }

    // ----- Save Video As… ------------------------------------------------
    std::filesystem::path save_src;
    std::error_code ec;
    if (!entry.cached_path.empty()) {
        const std::filesystem::path cp(entry.cached_path);
        if (std::filesystem::exists(cp, ec))
            save_src = cp;
    }
    if (save_src.empty()) {
        const std::filesystem::path sp(entry.source);
        if (std::filesystem::exists(sp, ec) && VideoPlayer::is_video_path(sp))
            save_src = sp;
    }

    const bool can_save = !save_src.empty();
    if (!can_save)
        ImGui::BeginDisabled();

    if (ImGui::MenuItem("Save Video As\xe2\x80\xa6")) {
        const std::string suggested = save_src.filename().string();
        static const SDL_DialogFileFilter filters[] = {
            {"Video files", "mp4;mkv;avi;mov;webm;flv;wmv;m4v"},
            {"All files",   "*"},
        };
        copy_source = save_src;
        copy_dest.clear();
        SDL_ShowSaveFileDialog(VideoContextMenu::save_dialog_callback,
                               self, window, filters, 2, suggested.c_str());
    }

    if (!can_save)
        ImGui::EndDisabled();

    return result;
}

VideoContextMenu::Result VideoContextMenu::draw_menu_items(
    const WindowStateToml::ImageHistoryEntry &entry)
{
    return draw_menu_body(this, m_window, m_copy_source, m_copy_dest, entry);
}

// ---------------------------------------------------------------------------
// draw_for_item
// ---------------------------------------------------------------------------

VideoContextMenu::Result VideoContextMenu::draw_for_item(
    const WindowStateToml::ImageHistoryEntry &entry)
{
    Result result;
    if (!ImGui::BeginPopupContextItem())
        return result;
    result = draw_menu_body(this, m_window, m_copy_source, m_copy_dest, entry);
    ImGui::EndPopup();
    return result;
}

// ---------------------------------------------------------------------------
// draw_for_window
// ---------------------------------------------------------------------------

VideoContextMenu::Result VideoContextMenu::draw_for_window(
    const WindowStateToml::ImageHistoryEntry &entry,
    const char *popup_id)
{
    Result result;
    if (!ImGui::BeginPopupContextWindow(popup_id))
        return result;
    result = draw_menu_body(this, m_window, m_copy_source, m_copy_dest, entry);
    ImGui::EndPopup();
    return result;
}

// ---------------------------------------------------------------------------
// process_pending_save
// ---------------------------------------------------------------------------

void VideoContextMenu::process_pending_save()
{
    if (m_copy_dest.empty())
        return;

    const auto dest   = m_copy_dest;
    const auto source = m_copy_source;
    m_copy_dest.clear();
    m_copy_source.clear();

    if (source.empty()) {
        std::println("[VideoContextMenu] save cancelled (no source)");
        return;
    }

    std::error_code ec;
    std::filesystem::copy_file(source, dest,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec)
        std::println("[VideoContextMenu] save failed {}: {}", dest.string(), ec.message());
    else
        std::println("[VideoContextMenu] saved: {}", dest.string());
}
