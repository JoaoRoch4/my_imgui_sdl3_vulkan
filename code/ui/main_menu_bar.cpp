/**
 * @file main_menu_bar.cpp
 * @brief Implementation of MainMenuBar.
 *
 * This file handles:
 *   - File dialog and URL dialog management.
 *   - Decoding / downloading images and handing them to ImageViewerPanel.
 *   - Rendering the ImGui menu bar and the "Open Online" popup.
 *   - History persistence (up to 100 entries, 20 shown at once).
 *
 * Image rendering (zoom, pan, per-window draw) is fully delegated to
 * ImageViewerPanel::draw_windows() — see image_viewer_panel.cpp.
 */

#include "main_menu_bar.hpp"
#include "Image_viewer_panel.hpp"
#include "video_player.hpp"

#include "imgui.h"
#include "style_editor.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

// ============================================================================
// Anonymous-namespace helpers (file-local, not part of any class)
// ============================================================================

namespace {

/// Buffer accumulator used by the libcurl write callback.
struct CurlBuf {
    std::vector<uint8_t> data;
};

/**
 * @brief libcurl write callback — appends downloaded bytes to a CurlBuf.
 *
 * @param ptr    Pointer to the data block delivered by curl.
 * @param size   Always 1 (curl convention).
 * @param nmemb  Number of bytes in the block.
 * @param user   Pointer to our CurlBuf, passed via CURLOPT_WRITEDATA.
 * @return       Number of bytes handled (must equal size * nmemb on success).
 */
static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *user) {
    auto *buf = static_cast<CurlBuf *>(user);                       /// Cast the user pointer to our buffer.
    const auto *bytes = static_cast<const uint8_t *>(ptr);          /// Cast the data pointer to bytes.
    buf->data.insert(buf->data.end(), bytes, bytes + size * nmemb); /// Append to accumulator.
    return size * nmemb;                                            /// Tell curl we consumed everything.
}

/**
 * @brief Derive a file extension from a URL path component.
 *
 * Strips the query string first (everything from '?' onward), then
 * extracts the extension with std::filesystem::path.  Falls back to
 * ".jpg" if the extension is not in the recognised list.
 *
 * @param url  Full URL string (may contain a query string).
 * @return     Extension string including the leading dot, e.g. ".png".
 */
static std::string ext_from_url(const std::string &url) {
    /// Remove the query string (e.g. "?size=large") before path parsing.
    const std::string clean = url.substr(0, url.find('?'));

    /// Extract the extension from the path component of the (cleaned) URL.
    std::string ext = std::filesystem::path(clean).extension().string();

    /// Allowlist of extensions the texture loader can handle.
    constexpr std::array<std::string_view, 7> valid{
        ".jpg", ".jpeg", ".png", ".bmp", ".tga", ".gif", ".webp"};

    /// Return the extension only if it is in the allowlist.
    for (auto v : valid)
        if (ext == v)
            return ext;

    return ".jpg"; /// Default fallback.
}

/**
 * @brief Download a URL into a uniquely named temp file.
 *
 * The file is named with a random suffix via mkstemp() and the correct
 * image extension appended so the decoder can detect the format.
 *
 * @param url  URL to download.
 * @return     Path to the temp file on success, or an empty path on failure.
 *             The CALLER is responsible for deleting the file.
 */
static std::filesystem::path download_to_temp(const std::string &url) {
    CurlBuf buf;

    /// Initialise a libcurl easy handle.
    CURL *curl = curl_easy_init();
    if (!curl)
        return {}; /// curl failed to initialise.

    /// Configure the transfer.
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());             /// Target URL.
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb); /// Our write callback.
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);              /// Accumulator argument.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);           /// Follow HTTP redirects.
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);                 /// 30-second hard timeout.

    const CURLcode res = curl_easy_perform(curl); /// Execute the download.
    curl_easy_cleanup(curl);                      /// Always free the handle.

    if (res != CURLE_OK || buf.data.empty())
        return {}; /// Download failed or produced no data.

    /**
     * Create a temp file with a random name.
     * mkstemp() opens the file and returns an fd; we close it immediately
     * and rename to add the correct extension.
     */
    char tmp_tpl[] = "/tmp/imgview_XXXXXX";
    const int fd = mkstemp(tmp_tpl);
    if (fd < 0)
        return {}; /// mkstemp failed (e.g. /tmp is full or read-only).
    close(fd);     /// We only needed the unique name, not the open fd.

    /// Append the image extension to the random name.
    std::filesystem::path final_path = std::string(tmp_tpl) + ext_from_url(url);

    /// Rename the empty placeholder to the final path.
    std::error_code ec;
    std::filesystem::rename(tmp_tpl, final_path, ec);
    if (ec)
        return {}; /// Rename failed.

    /// Write the downloaded bytes into the final path.
    std::ofstream ofs(final_path, std::ios::binary);
    if (!ofs) {
        std::filesystem::remove(final_path); /// Clean up on failure.
        return {};
    }
    ofs.write(reinterpret_cast<const char *>(buf.data.data()), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
              static_cast<std::streamsize>(buf.data.size()));

    return final_path;
}

/**
 * @brief Extract a human-readable title from a URL (the filename part).
 *
 * @param url  Full URL string.
 * @return     Filename component of the URL path (e.g. "cat.jpg"),
 *             or "online_image" as a fallback.
 */
static std::string title_from_url(const std::string &url) {
    const std::string clean = url.substr(0, url.find('?')); /// Strip query string.
    auto t = std::filesystem::path(clean).filename().string();
    return t.empty() ? "online_image" : t;
}

} // namespace

// ============================================================================
// Construction
// ============================================================================

/**
 * @brief Default constructor — zero-initialises all pointer members.
 */
MainMenuBar::MainMenuBar()
    : request_quit{false}
    , m_style_editor{nullptr}
    , m_window{nullptr}
    , m_vk{nullptr}
    , m_show_demo_window{nullptr}
    , m_show_another_window{nullptr}
    , m_viewer{} /// Default-construct the image panel.
    , m_history{}
    , m_open_image_dialogs{}
    , m_bulk_image_open{}
    , m_video_player{}
    , m_history_preview{}
    , m_opened_files_window{} {
}

// ============================================================================
// Public lifecycle
// ============================================================================

/**
 * @brief Store external dependencies and initialise libcurl.
 *
 * @param style_editor        Pointer to the style-editor window (may be null).
 * @param window              The SDL3 parent window (for file dialogs).
 * @param vk                  Active Vulkan context.
 * @param show_demo_window    Pointer to the ImGui demo-window toggle.
 * @param show_another_window Pointer to the "Another Window" toggle.
 */
void MainMenuBar::Setup(StyleEditor *style_editor,
                        SDL_Window *window,
                        vulkan_context *vk,
                        bool *show_demo_window,
                        bool *show_another_window) {
    m_style_editor = style_editor;
    m_window = window;
    m_vk = vk;
    m_show_demo_window = show_demo_window;
    m_show_another_window = show_another_window;

    /// Initialise the libcurl global state once per process.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    m_open_image_dialogs.setup(m_window);
    m_video_player.setup(m_vk);
    m_history_preview.setup(m_vk, &m_video_player, &m_viewer);
}

/**
 * @brief Shut down the image panel and libcurl.
 *
 * Must be called before ImGui_ImplVulkan_Shutdown().
 */
void MainMenuBar::Shutdown() {
    if (!m_vk)
        return;

    m_bulk_image_open.shutdown();

    m_video_player.shutdown();

    m_history_preview.shutdown();

    /// Let ImageViewerPanel free all VkImage / VkSampler / etc. resources.
    m_viewer.shutdown(*m_vk);

    /// Release libcurl global state (matches the curl_global_init in Setup).
    curl_global_cleanup();
}

// ============================================================================
// History persistence
// ============================================================================

/**
 * @brief Restore image history from the persisted window state.
 * @param state  Loaded TOML state object.
 */
void MainMenuBar::ApplyHistory(const WindowStateToml &state) {
    m_history = state.image_history; /// Replace in-memory history with the saved list.
    m_opened_files_window.apply_history(state);
}

void MainMenuBar::ApplyRuntimeConfig(const WindowStateToml &state) {
    m_config_runtime.ApplyLayout(state);
}

bool MainMenuBar::LoadOpenedFilesHistoryFromToml(const std::filesystem::path &file_path) {
    const bool loaded = m_opened_files_window.load_history_from_toml(file_path);
    if (loaded) {
        WindowStateToml state;
        if (LoadWindowStateToml(file_path, state))
            m_history = state.image_history;
    }
    return loaded;
}

/**
 * @brief Serialise image history into the window state for saving.
 *
 * Caps at 100 entries to keep the TOML file manageable.
 *
 * @param state  Output TOML state object.
 */
void MainMenuBar::ExportHistory(WindowStateToml *state) const {
    constexpr size_t k_max = 100; /// Maximum history entries written to disk.

    const size_t count = std::min(k_max, m_history.size()); /// Don't exceed the vector size.

    state->image_history = std::vector<WindowStateToml::ImageHistoryEntry>(
        m_history.begin(),
        m_history.begin() + static_cast<std::ptrdiff_t>(count));
}

void MainMenuBar::ExportRuntimeConfig(WindowStateToml *state) const {
    m_config_runtime.ExportLayout(state);
}

// ============================================================================
// Private helpers
// ============================================================================

/**
 * @brief Write the current local time as "YYYY-MM-DDTHH:MM:SS" into dst.
 * @param dst  Caller-supplied 20-byte array (including the null terminator).
 */
void MainMenuBar::current_timestamp(std::array<char, 20> &dst) {
    /// Capture the current wall-clock time.
    const auto now_c = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());

    /// Break the time_t into calendar fields using the thread-safe reentrant variant.
    struct tm tm_val{};
    localtime_r(&now_c, &tm_val);

    /// Format into the fixed-size buffer.
    std::strftime(dst.data(), dst.size(), "%Y-%m-%dT%H:%M:%S", &tm_val);
}

/**
 * @brief Insert one entry at the front of the history list.
 *
 * @param source  File path or URL string.
 * @param kind    "file" or "url".
 */
void MainMenuBar::push_history(const std::string &source, const std::string &kind) {
    std::array<char, 20> ts{};
    current_timestamp(ts); /// Generate the timestamp string.

    /// Remove any existing entry with the same source so there are no duplicates.
    std::erase_if(m_history, [&source](const WindowStateToml::ImageHistoryEntry& e) {
        return e.source == source;
    });

    /// Insert at the front so the most recent item appears first.
    m_history.insert(m_history.begin(),
                     WindowStateToml::ImageHistoryEntry{source, kind, ts.data()});
    m_opened_files_window.sync_history(m_history);
}

void ImageViewerPanel::evict_closed(vulkan_context &vk) {
    // First, identify if we actually have anything to delete
    auto it = std::find_if(m_images.begin(), m_images.end(),
                           [](const ImageEntry &e) { return !e.open; });

    if (it == m_images.end())
        return;

    // Safety: Wait for the RTX 3060 to finish pending frames
    vkDeviceWaitIdle(vk.device);

    for (auto &entry : m_images) {
        if (!entry.open && entry.texture.is_loaded()) {
            // Unload must call ImGui_ImplVulkan_RemoveTexture and vkDestroyImage
            entry.texture.unload(vk);
        }
    }

    // Clean up the vector
    std::erase_if(m_images, [](const ImageEntry &e) { return !e.open; });
}
// ============================================================================
// Per-frame build
// ============================================================================

/**
 * @brief Render the main menu bar and coordinate image loading each frame.
 *
 * Call order inside one ImGui frame:
 *   1. evict_closed()          — free GPU resources for closed images.
 *   2. Load pending file paths — hand them to ImageViewerPanel.
 *   3. Download pending URLs   — hand them to ImageViewerPanel.
 *   4. Draw the menu bar       — File, View menus and sub-menus.
 *   5. Draw the URL popup      — modal dialog for "Open Online...".
 *   6. draw_windows()          — delegate to ImageViewerPanel.
 */
void MainMenuBar::Build() {
    // -------------------------------------------------------------------------
    // Step 1 — evict closed images
    // -------------------------------------------------------------------------

    /**
     * Remove images whose open flag was cleared by the ImGui [x] button.
     * This runs FIRST, before any loading, so that closed slots free up
     * capacity for new images in the same frame.
     *
     * The frame fence was already waited on by the Vulkan backend before
     * NewFrame(), so the GPU is no longer reading last frame's resources.
     */
    if (m_vk)
        m_viewer.evict_closed(*m_vk);

    // -------------------------------------------------------------------------
    // Step 2 — load images from the file dialog queue
    // -------------------------------------------------------------------------

    /**
     * file_dialog_callback() populates m_pending_paths on the main thread.
     * We drain the queue here — at most one full file-dialog batch per frame.
     */
    std::vector<std::string> pending_paths = m_open_image_dialogs.take_pending_paths();
    if (!pending_paths.empty() && m_vk) {

        if (pending_paths.size() > 1) {
            // Split the batch: route videos to VideoPlayer, images to bulk queue
            std::vector<std::string> image_paths;
            for (const auto &path : pending_paths) {
                if (VideoPlayer::is_video_path(path)) {
                    m_video_player.add_from_path(path);
                    push_history(path, "file");
                } else {
                    image_paths.push_back(path);
                }
            }
            if (!image_paths.empty())
                m_bulk_image_open.enqueue_batch(std::move(image_paths));
        } else {
            for (const auto &path : pending_paths) {
                if (VideoPlayer::is_video_path(path)) {
                    if (m_video_player.add_from_path(path))
                        push_history(path, "file");
                } else {
                    if (m_viewer.add_from_path(path, *m_vk))
                        push_history(path, "file");
                }
            }
        }

    }

    // Process at most one validated batch item per frame to avoid descriptor churn.
    if (m_vk) {
        std::string next_path;
        m_bulk_image_open.try_pop_ready(&next_path);

        if (!next_path.empty()) {
            if (VideoPlayer::is_video_path(next_path)) {
                if (m_video_player.add_from_path(next_path))
                    push_history(next_path, "file");
            } else {
                if (m_viewer.add_from_path(next_path, *m_vk))
                    push_history(next_path, "file");
            }
        }
    }

    // -------------------------------------------------------------------------
    // Step 3 — download and load images from the URL queue
    // -------------------------------------------------------------------------

    std::vector<std::string> pending_urls = m_open_image_dialogs.take_pending_urls();
    if (!pending_urls.empty() && m_vk) {
        for (const auto &url : pending_urls) {
            // Video URLs are streamed directly by mpv (no download needed)
            if (VideoPlayer::is_video_url(url)) {
                const std::string title = title_from_url(url);
                if (m_video_player.add_from_url(url, title))
                    push_history(url, "url");
                continue;
            }

            /// Blocking download — writes bytes to a temp file with the right extension.
            const std::filesystem::path tmp = download_to_temp(url);

            if (!tmp.empty()) {
                /**
                 * Derive the display title from the URL's filename component
                 * (e.g. "https://example.com/cat.jpg?q=1" → "cat.jpg").
                 */
                const std::string title = title_from_url(url);

                /**
                 * Hand the temp file to ImageViewerPanel.
                 * ImageViewerPanel reads the file; we delete it afterward.
                 */
                if (m_viewer.add_from_url_temp(tmp, title, url, *m_vk))
                    push_history(url, "url");

                std::filesystem::remove(tmp); /// Clean up the temp file.
            }
        }
    }

    // -------------------------------------------------------------------------
    // Step 4 — menu bar
    // -------------------------------------------------------------------------

    if (!ImGui::BeginMainMenuBar())
        return; /// Menu bar not visible (e.g. fullscreen game mode).

    // --- File menu -----------------------------------------------------------

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open Image...", "Ctrl+O"))
            m_open_image_dialogs.begin_open_image_dialog();

        if (ImGui::MenuItem("Open Online..."))
            m_open_image_dialogs.open_online_popup();

        // --- Recent sub-menu -------------------------------------------------

        if (!m_history.empty() && ImGui::BeginMenu("Recent")) {
            constexpr int k_max_shown = 20; /// Maximum history items shown at once.
            int shown = 0;

            for (const auto &hentry : m_history) {
                if (shown >= k_max_shown)
                    break;
                ++shown;

                /**
                 * Build the menu-item label:
                 *   [file]  cat.jpg              (filename only for local files)
                 *   [url]   https://example...   (truncated to 60 chars)
                 */
                std::string label;
                if (hentry.kind == "file") {
                    label = "[file]  ";
                    label += std::filesystem::path(hentry.source).filename().string();
                } else {
                    label = "[url]   ";
                    label += hentry.source.size() > 60
                                 ? hentry.source.substr(0, 57) + "..."
                                 : hentry.source;
                }

                if (ImGui::MenuItem(label.c_str())) {
                    if (hentry.kind == "file")
                        m_open_image_dialogs.queue_path(hentry.source);
                    else
                        m_open_image_dialogs.queue_url(hentry.source);
                }

                /// Show the full source and timestamp as a tooltip on hover.
                if (ImGui::IsItemHovered())
                    m_history_preview.draw_for_hover(hentry);
            }

            /// If the history is longer than k_max_shown, indicate the overflow.
            if (shown < static_cast<int>(m_history.size())) {
                ImGui::Separator();
                ImGui::TextDisabled("(%zu more not shown)",
                                    m_history.size() - static_cast<size_t>(shown));
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Clear History")) {
                m_history.clear(); /// Wipe the in-memory history (persisted on next save).
                m_opened_files_window.sync_history(m_history);
            }

            ImGui::EndMenu();
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Quit", "Alt+F4"))
            request_quit = true; /// The main loop checks this flag.

        ImGui::EndMenu();
    }

    // --- View menu -----------------------------------------------------------

    if (ImGui::BeginMenu("View")) {
        /// Style editor toggle — only shown when one is attached.
        if (m_style_editor)
            ImGui::MenuItem("Style Editor", nullptr, &m_style_editor->IsOpen);

        if (m_show_demo_window)
            ImGui::MenuItem("Demo Window", nullptr, m_show_demo_window);

        if (m_show_another_window)
            ImGui::MenuItem("Another Window", nullptr, m_show_another_window);

        ImGui::MenuItem("Opened Files", nullptr, &m_opened_files_window.IsOpen);
        ImGui::MenuItem("Runtime Config", nullptr, &m_config_runtime.IsOpen);

        /**
         * Delegate image toggle items to the panel.
         * Each item lets the user re-show a minimised or closed window.
         */
        if (m_viewer.count() > 0) {
            ImGui::Separator();
            m_viewer.build_view_menu_items();
        }

        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();

    // -------------------------------------------------------------------------
    // Step 5 — URL popup
    // -------------------------------------------------------------------------

    /**
     * m_show_url_popup acts as a one-shot flag: we call OpenPopup here,
     * then immediately clear the flag so the popup opens exactly once.
     * OpenPopup must be called OUTSIDE of BeginPopupModal.
     */
    m_open_image_dialogs.draw_url_popup();

    // -------------------------------------------------------------------------
    // Step 6 — image viewer windows
    // -------------------------------------------------------------------------

    m_video_player.update_frames();
    m_config_runtime.Draw();

    /**
     * Delegate all per-image ImGui window rendering to ImageViewerPanel.
     * This draws one window per open image with zoom, pan, and black bg.
     */
    m_viewer.draw_windows();
    m_video_player.draw();

    int focus_id = -1;
    const auto activated = m_opened_files_window.draw(m_viewer, m_history_preview, &focus_id);
    if (focus_id >= 0)
        m_viewer.request_focus(focus_id);

    if (activated.has_value()) {
        if (activated->kind == "file") {
            m_open_image_dialogs.queue_path(activated->source);
        } else if (activated->kind == "url") {
            m_open_image_dialogs.queue_url(activated->source);
        }
    }
}